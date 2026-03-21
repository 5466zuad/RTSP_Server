cmake_minimum_required(VERSION 3.10)
project(RTSPServer)

# 1. 基础设置
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_AUTOMOC ON)

# 2. 寻找依赖库
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(Qt5 REQUIRED COMPONENTS Widgets)

# 用 pkg-config 寻找 FFmpeg 和 x264
pkg_check_modules(FFMPEG REQUIRED libavformat libavcodec libavdevice libswscale libavutil)
pkg_check_modules(X264 REQUIRED x264)

# 3. 源文件（纯无界面模式，去掉 Qt/MainWindow）
add_executable(rtsp_server
    src/main.cpp
    src/MainWindow.cpp
    src/RtspPlayer.cpp
    src/VideoWidget.cpp
    src/TcpServer.cpp
    src/RtpSender.cpp
    src/CameraCapture.cpp
)

add_executable(rtsp_server_cli
    src/cli_main.cpp
    src/TcpServer.cpp
    src/RtpSender.cpp
    src/CameraCapture.cpp
)

# 4. 引入头文件目录
target_include_directories(rtsp_server PRIVATE
    include
    ${FFMPEG_INCLUDE_DIRS}
    ${X264_INCLUDE_DIRS}
)

target_include_directories(rtsp_server_cli PRIVATE
    include
    ${FFMPEG_INCLUDE_DIRS}
    ${X264_INCLUDE_DIRS}
)

# 5. 链接库
target_link_libraries(rtsp_server PRIVATE
    Threads::Threads
    Qt5::Widgets
    ${FFMPEG_LIBRARIES}
    ${X264_LIBRARIES}
)

target_link_libraries(rtsp_server_cli PRIVATE
    Threads::Threads
    ${FFMPEG_LIBRARIES}
    ${X264_LIBRARIES}
)