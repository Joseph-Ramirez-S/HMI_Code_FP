cmake_minimum_required(VERSION 3.16)
project(HMI_SDL2 LANGUAGES C CXX)

find_package(SDL2 CONFIG REQUIRED)

add_executable(hmi_sdl2 main.cpp ../Bridge/engine_bridge.cpp ../Bridge/music_stream.c)
target_compile_features(hmi_sdl2 PRIVATE c_std_99 cxx_std_11)
if(TARGET SDL2::SDL2main)
    target_link_libraries(hmi_sdl2 PRIVATE SDL2::SDL2main)
endif()
target_link_libraries(hmi_sdl2 PRIVATE SDL2::SDL2)

add_custom_command(TARGET hmi_sdl2 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/../Bridge/assets/graphics.bin"
        "$<TARGET_FILE_DIR:hmi_sdl2>/graphics.bin"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/../sdcard"
        "$<TARGET_FILE_DIR:hmi_sdl2>/sdcard"
    VERBATIM)

get_target_property(sdl2_library_type SDL2::SDL2 TYPE)
if(WIN32 AND sdl2_library_type STREQUAL "SHARED_LIBRARY")
    add_custom_command(TARGET hmi_sdl2 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:SDL2::SDL2>" "$<TARGET_FILE_DIR:hmi_sdl2>"
        VERBATIM)
endif()
