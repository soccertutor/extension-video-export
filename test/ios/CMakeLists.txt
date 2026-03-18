cmake_minimum_required(VERSION 3.18)
project(ios_encoder_test LANGUAGES CXX OBJCXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_OBJCXX_STANDARD 17)

add_executable(ios_encoder_test
    ios_encoder_test.mm
    ../../native/AVAssetWriterEncoder.mm
)

target_include_directories(ios_encoder_test PRIVATE
    ../../native
    ../../project/common
)

set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0")

target_compile_definitions(ios_encoder_test PRIVATE
    GLES_SILENCE_DEPRECATION
)

target_link_libraries(ios_encoder_test PRIVATE
    "-framework AVFoundation"
    "-framework CoreFoundation"
    "-framework CoreMedia"
    "-framework CoreVideo"
    "-framework Foundation"
    "-framework IOSurface"
    "-framework Metal"
    "-framework OpenGLES"
)
