cmake_minimum_required(VERSION 3.20)
project(audio_flow C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 复用本地编译的 FFmpeg (同 video_flow)
set(ENV{PKG_CONFIG_PATH} "${CMAKE_SOURCE_DIR}/../../ffmpeg-build/lib/pkgconfig")

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat
    libavcodec
    libavutil
    libswresample
)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)

add_executable(audio_flow main.c)
target_link_libraries(audio_flow PRIVATE PkgConfig::FFMPEG PkgConfig::SDL2)
