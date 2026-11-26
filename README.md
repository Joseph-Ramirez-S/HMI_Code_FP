# HMI de ascensor de tres niveles

Interfaz táctil en estilo pixel art para el CrowPanel de 7 pulgadas con ESP32-P4. Presenta tres vestíbulos, una cabina de ascensor y un selector de pisos por escalera. El firmware, desarrollado con ESP-IDF 5.5, comparte con el simulador SDL2 la lógica de navegación, solicitudes de piso, animaciones y audio.

El CrowPanel muestra el estado del ascensor, lee las confirmaciones de piso y envía las solicitudes del usuario por GPIO. En la integración con el ascensor real, el PLC recibe esas solicitudes y se encarga del movimiento, las puertas físicas, los enclavamientos y la seguridad del equipo.

## Pines GPIO

Los números corresponden a los GPIO del ESP32-P4. Las asignaciones y la configuración están en [`main/main.c`](main/main.c).

### GPIO_IN · Confirmación de piso

| Señal | GPIO | Función |
| --- | --- | --- |
| `GPIO_IN_LEVEL_1` | **25** | Final de carrera del nivel 1 |
| `GPIO_IN_LEVEL_2` | **27** | Final de carrera del nivel 2 |
| `GPIO_IN_LEVEL_3` | **28** | Final de carrera del nivel 3 |

Las entradas son activas en `HIGH` y tienen resistencia *pull-down* interna. La HMI confirma un cambio después de tres lecturas consecutivas iguales. Un solo contacto activo identifica el piso. Ningún contacto activo indica que no hay un piso confirmado y varios contactos activos se tratan como una lectura ambigua.

### GPIO_OUT · Solicitudes de piso

| Señal | GPIO | Función |
| --- | --- | --- |
| `GPIO_OUT_LEVEL_1` | **50** | Solicitar el nivel 1 |
| `GPIO_OUT_LEVEL_2` | **51** | Solicitar el nivel 2 |
| `GPIO_OUT_LEVEL_3` | **53** | Solicitar el nivel 3 |

Las salidas comienzan en `LOW` y se activan en `HIGH`. El firmware mantiene una sola salida activa a la vez, incluida la de emergencia. Las solicitudes programadas de cabina y las llamadas exteriores con temporizador usan un pulso de hasta 200 ms. Una llamada exterior inmediata mantiene la salida mientras se sostiene la pulsación o hasta que se confirma el piso solicitado.

### GPIO_ALARM · Botón de emergencia

| Señal | GPIO | Función |
| --- | --- | --- |
| `GPIO_OUT_EMERGENCY` | **5** | Enviar al PLC la solicitud de parada desde el botón táctil de Emergencia |

`GPIO_ALARM` corresponde a la salida `GPIO_OUT_EMERGENCY` en el firmware. Es activa en `HIGH`, tiene prioridad sobre las solicitudes de piso y permanece activa durante la pulsación y por un mínimo de 200 ms. La cadena de seguridad y la parada física corresponden al equipo y su sistema de control.

## Interactividad exterior

| Control | Comportamiento |
| --- | --- |
| **Escalera** · esquina superior izquierda | Abre con un fundido el selector **Level 3**, **Level 2** y **Level 1**. Cada botón usa el color de su escenario y un círculo amarillo marca el vestíbulo actual. Seleccionar otro piso cambia el vestíbulo en pantalla. La flecha de regreso o el nivel actual cierran el selector. La navegación por escalera deja la posición física del ascensor bajo el control del PLC. |
| **Puerta del ascensor** | Si la cabina está en el mismo piso, la puerta se abre y la vista pasa al interior. Si está en otro piso, el toque solicita la cabina al nivel del vestíbulo y reproduce un aviso. La apertura espera la confirmación de ese piso. |
| **Indicadores exteriores** | Muestran el piso confirmado y el sentido de marcha. El rombo verde indica reposo y parpadea durante la espera de una llamada remota con `TIMER.DOOR`. Al arrancar aparecen las indicaciones de subida o bajada. |
| **Flecha de ayuda** | Tras 8 segundos sin interacción, una flecha verde en pixel art apunta a la puerta y rebota en bucle hasta el siguiente toque. Suena un aviso cuando aparece. |
| **Engranaje** · esquina superior derecha | Abre **SETTINGS** con una animación de despliegue. Las barras dan un aviso sonoro cuando cambia un valor. |
| **Bocina** · esquina inferior derecha | Silencia efectos y música a la vez. **SETTINGS** refleja ambos volúmenes en cero. Al desactivar el silencio se recuperan los niveles configurados. |

### SETTINGS

| Opción | Rango | Comportamiento |
| --- | --- | --- |
| `VFX` | 0–10 | Volumen de los efectos de sonido. 0 silencia y 10 aplica el nivel máximo. |
| `MUSIC` | 0–10 | Volumen de la música de fondo. 0 silencia y 10 aplica el nivel máximo. |
| `TIMER.DOOR` | 1–10 s | La casilla habilita la espera antes de solicitar un viaje. En el exterior solo se aplica si la cabina está en otro piso. En el interior se aplica a las salidas y a las siguientes paradas de la cola. |
| `STACK.MODE` | 1–5 solicitudes | La casilla habilita una cola con capacidad ajustable. Las solicitudes se atienden en el orden en que se ingresaron. |

## Interactividad interior

| Control o estado | Comportamiento |
| --- | --- |
| **Botones 1, 2 y 3** | Seleccionan el piso. La HMI termina la animación de cierre antes de enviar la solicitud. Con `STACK.MODE` se pueden ingresar varias paradas y el icono de personas muestra la cantidad pendiente. Cada hueco liberado permite incorporar otra solicitud al final de la cola. |
| **Abrir y Cerrar** | Animan la puerta de la cabina. Si se pulsa **Abrir** antes de completar el cierre para una salida, la puerta se reabre, se cancela la salida con sus solicitudes pendientes y la cabina permanece en el piso. Ambos botones quedan bloqueados durante el viaje y la ralentización de llegada. |
| **En movimiento** | Se activa el indicador de subida o bajada, se anima el fondo del hueco con cadenas y luminarias y suena el efecto de marcha en bucle. Durante la espera de `TIMER.DOOR` parpadea el cuadro central del indicador interior. |
| **Llegada al destino** | El final de carrera del piso seleccionado activa el timbre y la ralentización visual. Después se abre la puerta automáticamente cuando el piso está confirmado. Si el contacto se interrumpe brevemente, la HMI conserva la llegada pendiente y reintenta la apertura al confirmar el mismo piso. Un piso intermedio puede aparecer en el indicador durante el viaje, sin activar el timbre de destino. |
| **Salir** | Tocar el umbral con la puerta abierta regresa al vestíbulo del piso alcanzado mediante un fundido. Al salir se cancelan las solicitudes pendientes de la cabina. |
| **Emergencia en movimiento** | Envía la solicitud de parada al PLC, anula el destino y vacía la cola. El indicador muestra `ERR`. Al soltar el botón, el primer piso que se seleccione será el nuevo destino. Si `STACK.MODE` está activo, las nuevas solicitudes se agregan en orden detrás de ese destino. |
| **Emergencia en un piso confirmado** | Mantiene visible el número de piso, cancela el temporizador y las solicitudes de la cola y deja abierta la puerta de la cabina. |

## Integración con el PLC

1. **HMI → PLC.** La HMI señala una solicitud de piso por `GPIO_OUT_LEVEL_1` a `GPIO_OUT_LEVEL_3` o una solicitud de parada por `GPIO_OUT_EMERGENCY`.
2. **PLC → HMI.** El PLC controla el ascensor real y proporciona la confirmación de piso mediante `GPIO_IN_LEVEL_1` a `GPIO_IN_LEVEL_3`. La HMI usa esa confirmación para actualizar los indicadores, reproducir la llegada y abrir la puerta en pantalla.
3. **Temporizador y cola.** `TIMER.DOOR` y `STACK.MODE` programan cuándo y en qué orden la HMI envía solicitudes. Para ofrecer el mismo comportamiento con el ascensor real, la programación del PLC debe coordinarse con la HMI. Las señales descritas comunican pisos y emergencia. Si el PLC también implementa temporización o cola, ambos programas deben acordar dónde se aplica cada espera y cómo se aceptan, completan o cancelan las solicitudes.

## Compilación y recursos

- [`main/`](main/) contiene la lógica de hardware, lectura táctil, GPIO, gráficos y audio. [`peripheral/`](peripheral/) contiene los componentes de la placa.
- `Bridge/` contiene el motor compartido y el paquete RGB565 `assets/graphics.bin`, que se graba en la partición `graphics` definida en [`partitions.csv`](partitions.csv). `SDL2_port_game/` contiene el simulador de escritorio. Ambos directorios están al lado de `HMI_Code_FP/` en el espacio de trabajo original.
- Para compilar una copia de este repositorio, coloca `Bridge/` y su paquete gráfico al lado de `HMI_Code_FP/`. Con ESP-IDF 5.5 activado y desde `HMI_Code_FP/`, ejecuta:

  ```sh
  idf.py build
  idf.py flash monitor
  ```

  Si hay más de un puerto serie, indica el del CrowPanel a `idf.py` mediante `-p`.

### Audio en microSD

La tarjeta se monta por SDMMC en `/sdcard`. Coloca los audios en la raíz de una microSD con FAT32. Los efectos usan WAV PCM de 16 bits a 48 kHz, mono o estéreo. `Background_Sound.mp3` se lee por bloques y se reproduce en bucle después de la intro.

| Archivo | Uso |
| --- | --- |
| `INTRO.WAV` | Timbre sincronizado con la introducción y sonido de apertura. De aquí se extrae el efecto para las puertas exteriores e interiores. El cierre lo reproduce en sentido inverso. |
| `BUTTONS.WAV` | Pulsaciones de los controles de la interfaz. |
| `ARRIVAL.WAV` | Timbre de llegada. |
| `S1_BF.WAV` | Aviso de la flecha de ayuda. |
| `SCENE.WAV` | Efecto del fundido entre escenarios. |
| `CALL.WAV` | Aviso al solicitar el ascensor desde otro piso. |
| `ALERT.WAV` | Aviso del botón de emergencia. |
| `MOTION.WAV` | Bucle de movimiento y desaceleración de la cabina. |
| `BACK.WAV` | Regreso desde el selector de la escalera. |
| `OPTIONS.WAV` | Apertura de **SETTINGS**. |
| `SLIDER.WAV` | Cambios de valor en las barras de **SETTINGS**. |
| `Background_Sound.mp3` | Música de fondo. |
