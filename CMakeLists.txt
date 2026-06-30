cmake_minimum_required(VERSION 3.10)
project(EdgeVision-Agent)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV REQUIRED)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

include_directories(
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/third_party
    ${OpenCV_INCLUDE_DIRS}
)

add_executable(edge_agent 
    main.cpp
    hardware/v4l2_camera/V4L2Camera.cpp
    hardware/rknpu_infer/RKNNEngine.cpp
)

# 链接 OpenCV, Threads, 以及 RKNN 动态库
target_link_libraries(edge_agent 
    ${OpenCV_LIBS}
    Threads::Threads
    ${CMAKE_SOURCE_DIR}/lib/librknnrt.so
)