file(GLOB SOURCES "*.c" "src/*.c")

idf_component_register(
    SRCS ${SOURCES} 
         "../../Bridge/engine_bridge.cpp"
    INCLUDE_DIRS "." "include" 
                 "../../Bridge"
    REQUIRES bsp_extra bsp_display bsp_illuminate bsp_i2c esp_timer spiffs
)