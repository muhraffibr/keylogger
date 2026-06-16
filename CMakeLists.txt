cmake_minimum_required(VERSION 3.16)

# Add extra component directory for Percepio TraceRecorder (local patched copy)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components/TraceRecorder/kernelports/ESP-IDF_FreeRTOS")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(Secure_Data_Logger)
