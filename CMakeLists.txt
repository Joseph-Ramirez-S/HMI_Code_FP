# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# 1. Le decimos al compilador dónde están tus componentes de hardware
set(EXTRA_COMPONENT_DIRS "peripheral")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(HMI_Code_FP)

# RGB565 is read directly from this flash partition by the Bridge.
set(graphics_pack "${CMAKE_CURRENT_LIST_DIR}/../Bridge/assets/graphics.bin")
configure_file("${graphics_pack}" "${CMAKE_BINARY_DIR}/graphics.bin" COPYONLY)
partition_table_get_partition_info(graphics_capacity "--partition-name graphics" "size")
file(SIZE "${graphics_pack}" graphics_size)
math(EXPR graphics_capacity "${graphics_capacity}")
if(graphics_size GREATER graphics_capacity)
    message(FATAL_ERROR "graphics.bin exceeds the graphics partition")
endif()

idf_component_get_property(main_args esptool_py FLASH_ARGS)
idf_component_get_property(sub_args esptool_py FLASH_SUB_ARGS)
esptool_py_flash_target(storage-flash "${main_args}" "${sub_args}")
esptool_py_flash_to_partition(storage-flash graphics "${CMAKE_BINARY_DIR}/graphics.bin")
esptool_py_flash_to_partition(flash graphics "${CMAKE_BINARY_DIR}/graphics.bin")
