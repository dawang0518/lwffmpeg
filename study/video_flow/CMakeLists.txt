cmake_minimum_required(VERSION 3.20)
project(video_flow C)

set(CMAKE_C_STANDARD 11)

# 使用 pkg-config 查找本地编译的 FFmpeg
set(ENV{PKG_CONFIG_PATH} "${CMAKE_SOURCE_DIR}/../../ffmpeg-build/lib/pkgconfig")
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat
    libavcodec
    libavutil
    libswscale
)

add_executable(video_flow main.c)

# --static 拿到完整的依赖链 (含 macOS 框架)
target_link_libraries(video_flow PkgConfig::FFMPEG)
