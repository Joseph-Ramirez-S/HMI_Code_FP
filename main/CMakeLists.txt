file(GLOB SOURCES "*.c" "src/*.c")

idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS "." "include"
    REQUIRES bsp_extra bsp_display bsp_illuminate bsp_i2c esp_timer
)