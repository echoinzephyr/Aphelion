cmake_minimum_required(VERSION 3.22)

option(APHELION_TOOLBOX "Build the Web toolbox firmware" ON)
if(APHELION_TOOLBOX)
    set(IDF_TARGET esp32s3 CACHE STRING "Aphelion target")
    set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.toolbox.defaults")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(aphelion)
