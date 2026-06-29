cmake_minimum_required(VERSION 3.10)
project(EdgeVision-Agent)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 寻找 OpenCV 4.7
find_package(OpenCV REQUIRED)

include_directories(
    ${CMAKE_SOURCE_DIR}
    ${OpenCV_INCLUDE_DIRS}
)

# 编译主程序
add_executable(edge_agent 
    main.cpp
    hardware/v4l2_camera/V4L2Camera.cpp
)

# 链接 OpenCV 库
target_link_libraries(edge_agent ${OpenCV_LIBS})