#!/bin/bash
# 获取当前脚本所在目录（项目根目录）
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$SCRIPT_DIR"

# 创建 build 目录（如果不存在）
mkdir -p build
cd build

# 执行 CMake 配置和编译
echo "============================="
echo "Running CMake..."
cmake ..
echo "============================="
echo "Running make..."
make -j$(nproc)

# 回到项目根目录运行程序
echo "============================="
cd ..
echo "Running edge_agent from project root..."
./build/edge_agent