#!/bin/bash

# 遇到任何报错立刻原地停止
set -e

echo "========================================"
echo "🧹 [1/4] 正在清理旧战场并配置环境..."

# ✨ 关键：先杀掉残留的旧进程，防止端口被占用
echo "🔪 清理残留进程..."
pkill -f rtsp_server 2>/dev/null || true
sleep 1
pkill -9 -f rtsp_server 2>/dev/null || true

mkdir -p build
cd build
rm -rf *

echo "----------------------------------------"
echo "🔨 [2/4] 配置 CMake 与构建参数..."
# 加上 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON 方便 VSCode 消灭红线
cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ..

echo "----------------------------------------"
echo "⚙️ [3/4] 开始火力全开编译 (make -j$(nproc))..."
make -j$(nproc)

echo "----------------------------------------"
echo "🚀 [4/4] 编译成功！即将启动服务..."
echo "========================================"

./rtsp_server