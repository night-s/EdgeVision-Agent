# EdgeVision-Agent：基于自研异步框架与 RK3568 的边缘智能感知系统

[![C++17](https://img.shields.io/badge/C++-17-blue)](https://en.cppreference.com/w/cpp/17)
[![RK3568](https://img.shields.io/badge/Platform-RK3568-green)](https://www.rock-chips.com/)
[![License](https://img.shields.io/badge/License-MIT-yellow)](LICENSE)

## 🚀 项目简介
针对边缘设备处理视频采集与 AI 决策时的并发瓶颈与内存碎片问题，**自研 C++ 异步运行时框架**，并基于 **RK3568 NPU** 实现端到端的视觉检测闭环。系统从零开始构建，不依赖任何第三方线程库或内存管理库，完全手写核心组件，是工业级边缘 AI 推理底座的缩影。

---

## 🧠 核心架构与亮点

### 1. 底层驱动：V4L2 零拷贝采集
- 直接基于 Linux V4L2 框架封装摄像头采集模块，采用 **MMAP** 机制实现内核到用户态的零拷贝，避免了 `memcpy` 开销。
- 通过 `v4l2-ctl` 排查并规避了 RK3568 ISP 空壳节点冲突，精准映射 USB 摄像头真实节点（`/dev/video10`），保证了采集链路的稳定。

### 2. 异步框架：自研线程池与安全队列
- 实现 **C++11 动态线程池**，支持任意返回值的任务投递（返回 `std::future`），用于并发的 NPU 推理和后处理。
- 设计 **基于条件变量的安全队列**（容量限制为 5 帧），实现生产者-消费者模型。当队列满时自动丢弃老帧，并触发回调归还内存池，完美解决背压（Backpressure）问题。

### 3. 内存管理：定长 Free-List 内存池
- 针对高频图像帧（640×480×3 = 921KB）的频繁分配，手写 **定长内存池**，系统启动时预分配 4 块内存，分配和释放均为 O(1) 操作，彻底消除了 `new/delete` 带来的内存碎片，分配耗时从微秒级降至纳秒级。

### 4. AI 部署：RKNN NPU 硬件加速
- 封装 **RKNN C API**，将 YOLOv5s INT8 量化模型部署至 RK3568 NPU。
- 实现 **Letterbox 预处理** 与 **NCHW 格式正确解析**，并加入 **NMS（非极大值抑制）** 后处理算法，单帧推理耗时稳定在 **54ms**（约 18 FPS），检测框精准、无重叠。

---

## 📊 性能指标
| 指标 | 数据 |
| :--- | :--- |
| **NPU 推理延迟** | ~54 ms / frame |
| **系统 FPS** | ~18 FPS |
| **内存分配耗时** | 从 `malloc` 微秒级 → 内存池纳秒级 |
| **丢帧回调** | 队列满时自动回调归还内存，无泄漏 |
| **稳定性** | 连续运行 1 小时无 OOM，无崩溃（Valgrind 验证） |

---

## 🛠️ 构建与运行
```bash
mkdir -p build && cd build
cmake ..
make -j$(nproc)
./edge_agent