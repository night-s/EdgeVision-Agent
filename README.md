# EdgeVision：RK3568 视频感知与事件录像
C++17/Linux 上的 V4L2 采集、YOLOv5/RKNN 检测、MPP H.264 编码、RTSP、ROI 事件录像与远程控制。推理与视频独立调度；推理落后时处理最新帧，过滤过期结果。

## 快速运行
需要板端 OpenCV、匹配的 RKNN runtime、GStreamer app/RTSP server 开发包和 MPP 插件，RGA 可选：
~~~sh
cmake -S . -B build-refactor -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/edgevision
cmake --build build-refactor -j2
(cd build-refactor && ctest --output-on-failure)
./build-refactor/edge_agent configs/default.json
~~~
默认请求 640×480 YUYV@30，设备 /dev/video10；建议按本机设备修改为 /dev/v4l/by-id/...-video-index0。请求帧率不等于实际帧率，见 [摄像头调查](docs/camera-investigation.md)。控制 9000，RTSP 8554，VLC 可打开 rtsp://板卡IP:8554/live。

[PC 演示与录像播放](docs/pc-viewer.md) · [构建与部署](docs/deployment.md)

## PC 检测可视化
Windows 本地仓库根目录：
~~~powershell
py -3.12 -m venv .venv-pc
.\.venv-pc\Scripts\python.exe -m pip install -r tools/requirements-pc.txt
.\.venv-pc\Scripts\python.exe tools/pc_viewer.py --host 192.168.94.126
~~~
左侧实时 RTSP，右侧同一来源帧的图像与检测框；显示 FPS、P95、推理健康、丢帧和录像状态。右侧为低频同帧预览，不声称与左侧码流逐帧同步。Q 退出、R 录像、S 抓拍。

## 实现与验证
- FramePool、BoundedQueue、VisionPipeline、EventLoop、VideoService 为当前核心链路；旧 MemoryPool/SafeQueue/SkillManager 等实现已删除。
- V4L2 协商格式/帧率检查，分开记录 poll、复制、驱动帧间隔与序号缺口。
- Letterbox、anchor 解码和 NMS；默认 logits，支持原生量化输出和浮点对照。
- 编码前显式转换 NV12；RTSP 视频不等待推理。
- GOP 预录、尾随触发、断流收尾、状态查询、录像配额及最长事件时长。
- 异步 JPEG 抓拍/同帧预览，半关闭连接仍可接收已接受任务结果。
- 慢处理降级、恢复滞回、有效期和停滞观测。

默认录像预算 256 MiB / 128 文件，预留 64 MiB 空间，单段最长 60 秒。只轮转完整且写入成功的结束事件，不完整事件保留；约每秒检查，非严格逐字节硬限额。JPEG 与 metrics 日志暂不在事件配额内。

~~~sh
python3 tools/edge_client.py get_metrics --host 板卡IP
python3 tools/edge_client.py capture --host 板卡IP
python3 tools/edge_client.py record_event --host 板卡IP
python3 tools/edge_client.py get_events --host 板卡IP
~~~
stop/start 只暂停/恢复推理；restart_camera 请求重开采集。
[协议](docs/protocol.md) · [架构](docs/architecture.md) · [慢推理保护](docs/inference-recovery.md) · [实测](docs/validation.md) · [P0 核查](docs/p0-review.md) · [面试资料](docs/interview/README.md)

同输入、视频开启的历史单轮 60 秒对照：SDK 浮点输出 11.74 FPS，原生量化输出 14.59 FPS；不是 NPU 本体提速或 30 FPS 检测。公开报告/配置见 docs/interview/evidence，原始图像和完整输出日志不纳入 Git。

## 边界
尚无推理子进程硬超时恢复、编码器自动重建、DMA-BUF 全链路共享、MP4/MKV、音频、跟踪或完整数据集 mAP。当前输入下 RGA 未显示收益，CPU 为默认预处理。检测延迟不包含曝光、网络和 PC 播放缓冲。控制接口用于受信任局域网，未实现认证/TLS。
