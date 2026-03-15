cmake_minimum_required(VERSION 3.18)
project(android_encoder_test LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)

add_executable(android_encoder_test
    android_encoder_test.cpp
    ../../native/mediacodec_encoder.cpp
)

target_compile_definitions(android_encoder_test PRIVATE __ANDROID__)
target_include_directories(android_encoder_test PRIVATE
    ../../native
    ../../project/common
)

find_library(MEDIANDK_LIB mediandk)
find_library(EGL_LIB EGL)
find_library(GLESV3_LIB GLESv3)
find_library(LOG_LIB log)
find_library(ANDROID_LIB android)

target_link_libraries(android_encoder_test PRIVATE
    ${MEDIANDK_LIB}
    ${EGL_LIB}
    ${GLESV3_LIB}
    ${LOG_LIB}
    ${ANDROID_LIB}
)
