idf_component_register(
    SRCS "main.c" "panel_graphics.c" "intro_audio.c" "audio_sd.c"
         "../Bridge/engine_bridge.cpp" "../Bridge/music_stream.c"
    INCLUDE_DIRS "." "include" 
                 "../Bridge"
    REQUIRES bsp_extra bsp_display bsp_illuminate bsp_i2c driver esp_timer esp_partition esp_lcd esp_mm esp_driver_ppa
    PRIV_REQUIRES fatfs sdmmc esp_driver_sdmmc esp_driver_i2s esp_driver_uart
)
# ESP-IDF 5.5 exposes its native 2D framebuffer copy engine to LCD components.
idf_component_get_property(lcd_dir esp_lcd COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE "${lcd_dir}/priv_include")
