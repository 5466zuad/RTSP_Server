cmake_minimum_required(VERSION 3.10)
project(RTSPServer)

# 1. 基础设置
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # 让 VSCode 别画红线

# 2. 开启 Qt 自动化 (MOC/UIC/RCC)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# 3. 寻找外援库 (多线程 + Qt5)
find_package(Threads REQUIRED)
find_package(Qt5 REQUIRED COMPONENTS Widgets Charts)

# 4. 🔥 核心修改在这里：把新文件加进编译名单！
# 注意路径要写对：src/xxx.cpp 和 include/xxx.h
add_executable(rtsp_server 
    src/main.cpp 
    src/TcpServer.cpp 
    src/RtpSender.cpp
    src/MainWindow.cpp   # 新来的 UI 实现
    include/MainWindow.h # 新来的 UI 头文件
    include/TcpServer.h  # 👈 💥 核心修复点：把带信号的服务器头文件加进来！
)

# 5. 链接库 (给程序装上 Qt、线程 的翅膀)
target_link_libraries(rtsp_server PRIVATE 
    Threads::Threads
    Qt5::Widgets
    Qt5::Charts
)