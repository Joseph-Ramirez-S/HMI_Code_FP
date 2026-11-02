# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# 1. Le decimos al compilador dónde están tus componentes de hardware
set(EXTRA_COMPONENT_DIRS "peripheral")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(HMI_Code_FP)

# Empaquetamos el fondo y la animacion usados por el renderizador.
set(SPIFFS_ASSETS_DIR "${CMAKE_BINARY_DIR}/spiffs_assets")
file(MAKE_DIRECTORY "${SPIFFS_ASSETS_DIR}")
foreach(asset roomde.png neko_strip6.png elevator_door_strip8.png Level_indicator.png elevator_inside.png)
    configure_file(
        "${CMAKE_CURRENT_LIST_DIR}/../Bridge/assets/${asset}"
        "${SPIFFS_ASSETS_DIR}/${asset}"
        COPYONLY
    )
endforeach()
# SPIFFS has a short filename limit; store this door sheet under an alias.
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/../Bridge/assets/elevator door2_strip8.png"
    "${SPIFFS_ASSETS_DIR}/inside_door.png"
    COPYONLY
)
# SPIFFS has a short filename limit; store this sprite sheet under an alias.
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/../Bridge/assets/agente_sombra_periodico_sprites.png"
    "${SPIFFS_ASSETS_DIR}/agente.png"
    COPYONLY
)
# La hoja original es demasiado grande para decodificarla como RGBA en PSRAM.
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/../Bridge/assets/la_que_atiende_strip4_esp32.png"
    "${SPIFFS_ASSETS_DIR}/la_que_atiende_strip4.png"
    COPYONLY
)
spiffs_create_partition_image(storage "${SPIFFS_ASSETS_DIR}" FLASH_IN_PROJECT)