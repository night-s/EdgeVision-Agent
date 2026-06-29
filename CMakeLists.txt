cmake_minimum_required(VERSION 3.10)
project(EdgeVision-Agent)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 寻找 OpenCV
find_package(OpenCV REQUIRED)

# 寻找 Threads (用于支持 std::thread 和 pthread)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

include_directories(
    ${CMAKE_SOURCE_DIR}
    ${OpenCV_INCLUDE_DIRS}
)

# 编译主程序
add_executable(edge_agent 
    main.cpp
    hardware/v4l2_camera/V4L2Camera.cpp
)

# 链接 OpenCV 和 线程库
target_link_libraries(edge_agent 
    ${OpenCV_LIBS}
    Threads::Threads
)