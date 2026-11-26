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

![Vestíbulo del nivel 1 con la escalera, el engranaje, la bocina y la flecha que apunta al ascensor](docs/capturas/vestibulo.png)

| Captura | Control | Qué hace |
| --- | --- | --- |
| <img src="docs/capturas/icono-escalera.png" width="76" alt="Escalera de madera en pixel art"> | **Escalera** | En la esquina superior izquierda. Al tocarla, la pantalla se oscurece y aparece el [selector de niveles](#selector-de-niveles). Sirve para visitar los pisos en pantalla sin llamar a la cabina. |
| <img src="docs/capturas/puerta-exterior.png" width="112" alt="Puerta del ascensor vista desde el vestíbulo"> | **Puerta del ascensor** | Si la cabina está en tu piso, toca la puerta para abrirla y entrar. Si está en otro piso, el toque pide que la cabina venga y suena un aviso. La puerta se abre cuando llega a tu piso. |
| <img src="docs/capturas/indicador-exterior.png" width="150" alt="Número de piso, rombo verde y flechas verdes hacia abajo"> | **Indicadores exteriores** | El número sobre la puerta muestra el piso confirmado de la cabina. El rombo verde indica que está parada. Si está contando el tiempo de `TIMER.DOOR`, el rombo parpadea. Cuando la cabina arranca, las flechas muestran si sube o baja. |
| <img src="docs/capturas/flecha-ayuda.png" width="88" alt="Flecha verde que apunta a la puerta"> | **Flecha de ayuda** | Si pasan 8 segundos sin tocar la pantalla, suena un aviso y la flecha empieza a rebotar apuntando a la puerta. Desaparece con el siguiente toque. |
| <img src="docs/capturas/engranaje.png" width="76" alt="Engranaje de configuración en pixel art"> | **Engranaje** | En la esquina superior derecha. Al tocarlo, gira un poco y abre [SETTINGS](#menú-settings). |
| <img src="docs/capturas/bocina.png" width="148" alt="Bocina con sonido y bocina silenciada con una X roja"> | **Bocina** | En la esquina inferior derecha. Un toque silencia los efectos y la música. Ambas barras muestran 0. Otro toque recupera los volúmenes anteriores. |

### Selector de niveles

<img src="docs/capturas/selector-niveles.png" width="500" alt="Menú Level 3, Level 2 y Level 1, con una flecha de regreso y un círculo amarillo al lado del nivel actual">

Toca **Level 3**, **Level 2** o **Level 1** para ir al pasillo de ese piso. La pantalla se oscurece un momento mientras cambia la vista. El círculo amarillo marca el piso que estás viendo. Toca ese mismo nivel o la flecha amarilla para volver al pasillo donde estabas.

### Menú SETTINGS

<img src="docs/capturas/settings.png" width="565" alt="Menú SETTINGS con las barras VFX y MUSIC y las casillas TIMER.DOOR y STACK.MODE activadas">

El menú baja suavemente al abrirse. Toca la **X** o el engranaje para cerrarlo. Cada vez que cambias el valor de una barra suena un aviso corto.

| Opción | Rango | Qué hace |
| --- | --- | --- |
| `VFX` | 0–10 | Ajusta el volumen de los efectos. En 0 quedan en silencio. |
| `MUSIC` | 0–10 | Ajusta el volumen de la música de fondo. En 0 queda en silencio. |
| `TIMER.DOOR` | 1–10 s | Marca la casilla para elegir cuántos segundos esperar antes de cada salida de la cabina. Desde el pasillo, solo hay espera cuando pides la cabina desde otro piso. |
| `STACK.MODE` | 1–5 solicitudes | Marca la casilla para elegir cuántas solicitudes caben en la cola. La cabina atiende los pisos en el orden en que se pidieron. |

## Interactividad interior

![Cabina con la puerta y el número de piso al centro, el panel de botones a la derecha y el contador arriba a la izquierda](docs/capturas/cabina.png)

| Captura | Control o estado | Qué hace |
| --- | --- | --- |
| <img src="docs/capturas/botones-piso.png" width="110" alt="Botones 3, 2 y 1 del panel interior"> | **Botones 1, 2 y 3** | Toca el piso al que quieres ir. La HMI manda la solicitud cuando la puerta termina de cerrarse en pantalla. |
| <img src="docs/capturas/contador-stack.png" width="132" alt="Icono de persona con el contador 2 de 5"> | **Contador de `STACK.MODE`** | Indica cuántas solicitudes están pendientes y cuántas caben en total. Por ejemplo, **2/5** significa que hay dos solicitudes de un máximo de cinco. Cuando se libera un lugar, puedes añadir otro piso al final de la cola. |
| <img src="docs/capturas/botones-puerta.png" width="148" alt="Botón Abrir a la izquierda y botón Cerrar a la derecha"> | **Abrir y Cerrar** | El botón izquierdo abre la puerta en pantalla y el derecho la cierra. Si tocas **Abrir** mientras la puerta se cierra para iniciar un viaje, se vuelve a abrir, se borran las solicitudes pendientes y la cabina se queda en ese piso. Los botones se bloquean durante el viaje y mientras la cabina se detiene al llegar. |
| <img src="docs/capturas/indicador-interior.png" width="148" alt="Número de piso y flechas rojas que indican que la cabina sube"> | **Indicador interior** | Muestra el número de piso y si la cabina sube o baja. Mientras viajas, las cadenas y las luces del fondo se mueven y suena el efecto de marcha. El cuadro central parpadea durante la cuenta de `TIMER.DOOR`. |
| <img src="docs/capturas/puerta-cabina.png" width="148" alt="Puerta de la cabina abierta al llegar al nivel 2"> | **Llegar y salir** | El timbre suena al llegar al piso pedido. El movimiento en pantalla se hace más lento y la puerta se abre sola cuando el sensor confirma el piso. Si se pierde la señal un momento, espera a que vuelva. Por ejemplo, del 1 al 3 el timbre suena solo al llegar al 3. Toca la puerta abierta para volver al pasillo de ese piso. La pantalla se oscurece un momento y se borran las solicitudes pendientes. |
| <img src="docs/capturas/emergencia.png" width="152" alt="Botón de emergencia presionado y pantalla interior que muestra ERR"> | **Emergencia** | **Si la cabina está viajando**, pide al PLC que la detenga, borra las solicitudes y muestra `ERR`. Al soltar el botón, puedes escoger otro piso. Con `STACK.MODE`, puedes añadir más solicitudes a esa nueva lista. **Si ya está parada en un piso confirmado**, cancela la espera y la cola, deja la puerta abierta y sigue mostrando el número del piso. |

## Integración con el PLC

1. **De la pantalla al PLC.** Cuando pides un piso, la HMI activa la salida de ese piso que aparece en la tabla de GPIO. El botón de Emergencia usa `GPIO_OUT_EMERGENCY` para pedir una parada.
2. **Del PLC a la pantalla.** El PLC controla el ascensor real. Cuando confirma un piso por la entrada correspondiente, la HMI actualiza los indicadores. Si la cabina llega al piso pedido, suena el timbre y se abre la puerta en pantalla.
3. **Esperas y pedidos en orden.** `TIMER.DOOR` y `STACK.MODE` organizan cuándo y en qué orden la HMI envía los pedidos. Si el PLC también tiene un temporizador o una cola, hay que coordinar ambos programas para que, por ejemplo, una misma espera se cuente una sola vez.

## Compilación y recursos

- En [`main/`](main/) está el código que conecta la pantalla táctil, los pines, los gráficos y el sonido. En [`peripheral/`](peripheral/) están los componentes de la placa.
- `Bridge/` guarda el código que comparten la HMI y el simulador `SDL2_port_game/`. También incluye `assets/graphics.bin`, el archivo de imágenes que se copia a la partición `graphics` de la placa durante la instalación. La partición se define en [`partitions.csv`](partitions.csv).
- Para compilar esta copia, coloca la carpeta `Bridge/` al lado de `HMI_Code_FP/`. Abre una terminal con ESP-IDF 5.5 activado y ejecuta estos comandos desde `HMI_Code_FP/`.

  ```sh
  idf.py build
  idf.py flash monitor
  ```

  Si hay varios puertos serie, usa `idf.py -p PUERTO flash monitor` y reemplaza `PUERTO` por el puerto del CrowPanel.

### Audio en microSD

Guarda los archivos directamente en una microSD con formato FAT32. La placa lee la tarjeta por SDMMC y los encuentra en `/sdcard`. Los efectos usan WAV PCM de 16 bits a 48 kHz, mono o estéreo. La música empieza al terminar la introducción y vuelve a empezar cuando llega al final. La placa la lee por partes mientras suena.

| Archivo | Uso |
| --- | --- |
| `INTRO.WAV` | Timbre sincronizado con la introducción y sonido de apertura. De aquí se extrae el efecto para las puertas exteriores e interiores. El cierre lo reproduce en sentido inverso. |
| `BUTTONS.WAV` | Pulsaciones de los controles de la interfaz. |
| `ARRIVAL.WAV` | Timbre de llegada. |
| `S1_BF.WAV` | Aviso de la flecha de ayuda. |
| `SCENE.WAV` | Sonido que acompaña el breve oscurecimiento al cambiar de pantalla. |
| `CALL.WAV` | Aviso al solicitar el ascensor desde otro piso. |
| `ALERT.WAV` | Aviso del botón de emergencia. |
| `MOTION.WAV` | Sonido que se repite mientras la cabina se mueve y baja de volumen cuando se detiene. |
| `BACK.WAV` | Regreso desde el selector de la escalera. |
| `OPTIONS.WAV` | Apertura de **SETTINGS**. |
| `SLIDER.WAV` | Cambios de valor en las barras de **SETTINGS**. |
| `Background_Sound.mp3` | Música de fondo. |
