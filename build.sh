#!/bin/bash

# 遇到任何报错立刻原地停止
set -e

echo "========================================"
echo "🧹 [1/4] 正在清理旧战场并配置环境..."

# ✨ 关键：先杀掉残留的旧进程，防止端口被占用
echo "🔪 清理残留进程..."
sudo kill -9 $(pgrep -f rtsp_server) 2>/dev/null || true
sleep 1

# ✨ 关键：在脚本里也声明一下显示器地址，防止 sudo 弄丢环境变量
export DISPLAY=$(grep -m 1 nameserver /etc/resolv.conf | awk '{print $2}'):0.0

mkdir -p build
cd build
rm -rf *

echo "----------------------------------------"
echo "🔨 [2/4] 召唤 CMake 链接 Qt5 与多线程..."
# 加上 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON 方便 VSCode 消灭红线
cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ..

echo "----------------------------------------"
echo "⚙️ [3/4] 开始火力全开编译 (make -j$(nproc))..."
make -j$(nproc)

echo "----------------------------------------"
echo "🚀 [4/4] 编译成功！图形界面即将跳出..."
echo "========================================"

# ✨ 核心提醒：sudo 会重置环境变量，所以我们要把 DISPLAY 传进去
# -E 参数表示保留当前用户的环境变量
sudo -E ./rtsp_server