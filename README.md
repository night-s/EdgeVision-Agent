# EdgeVision：RK3568 视频感知与事件录像系统

C++17/Linux 摄像头采集、YOLOv5 推理、MPP H.264 编码、RTSP 预览、远程控制和事件录像。
推理与视频使用独立有界队列，推理落后时丢弃旧的待处理帧，视频继续运行。

## 构建与运行

需要匹配的 OpenCV、RKNN runtime、GStreamer app/RTSP server 开发包、
Rockchip MPP 插件；RGA 可选。lib/librknnrt.so 与模型必须匹配。

~~~bash
cmake -S . -B build-refactor -DCMAKE_BUILD_TYPE=Release
cmake --build build-refactor -j2
(cd build-refactor && ctest --output-on-failure)
./build-refactor/edge_agent configs/default.json
~~~

仅 CPU 预处理可加 -DEDGE_WITH_RGA=OFF。有 OpenCV 的 Linux 主机可加
-DEDGE_BUILD_AGENT=OFF 仅构建基础测试。
tools/setup_multimedia_deps.sh 可将缺少的开发包解压至 .deps/，不替换系统运行库；
应先检查版本是否兼容。构建支持该目录及构建 RPATH。

默认 /dev/video10、640×480 YUYV、30 FPS；控制 9000，RTSP 8554。
PC 的 VLC 打开 rtsp://<板卡IP>:8554/live。当前预览为原始视频，
检测框通过接口返回，尚未叠加至编码画面。退出用 Ctrl+C。

~~~bash
python3 tools/edge_client.py get_status --host <板卡IP>
python3 tools/edge_client.py get_detections --host <板卡IP>
python3 tools/edge_client.py get_metrics --host <板卡IP>
python3 tools/edge_client.py capture --host <板卡IP>
python3 tools/edge_client.py set_threshold --value 0.5 --host <板卡IP>
python3 tools/edge_client.py stop --host <板卡IP>
python3 tools/edge_client.py start --host <板卡IP>
python3 tools/edge_client.py restart_camera --host <板卡IP>
python3 tools/edge_client.py record_event --host <板卡IP>
~~~

stop 仅暂停推理，视频继续。capture 无条件保存最近有效帧，返回板端文件路径；
不会自动下载图片。record_event 异步完成，结果见 output/events/*.h264.json。

## 功能与配置

- 固定容量帧池和只读共享引用；驱动缓冲拷贝有效行后立即 QBUF。
- 摄像头非阻塞取帧、超时重开；signalfd 退出、工作队列排空、资源回收。
- YOLOv5 anchor 解码、Letterbox 逆变换、同类别 NMS、输出布局校验。
- 当前仓库模型输出是 logits，默认 output_activation=logits。
- native_outputs=true 使用原生量化输出；false 可做浮点输出对照。
- preprocess=cpu/rga；当前 640×480 实测 RGA 未更快，默认 CPU。
- 人员框中心位于 roi 且连续 event_confirm 帧触发事件，event_pre/event_tail 控制前后缓存。
- 录像为可解码的 Annex-B H264 裸流及清单，尚无 MP4/MKV 封装和磁盘轮转。
- synthetic=true 使用合成输入；replay_yuyv 指向恰好一帧 packed YUYV，用于固定输入对照。
- 两种回放模式仍使用真实 NPU/编码器；非匹配布局模型不能直接替换。

## 验证

~~~bash
python3 tests/board_validation.py
python3 tools/benchmark.py --seconds 60 --backends cpu rga
./build-refactor/edge_model_validation models/yolov5s-640-640.rknn frame-640x480.yuyv output/model-report.json
~~~

[架构与边界](docs/architecture.md) · [协议](docs/protocol.md) · [实测记录](docs/validation.md)

output/ 保存配置、日志、指标、抓拍和录像，不纳入 Git。耗时分位数保留最近 1024 次观察，
CPU 以单核 100% 为口径。本地采集完成至检测/编码完成不包含曝光和 PC 播放。
服务尚无认证/TLS，用于受信任局域网。DeepSeek 是可选控制入口，不参与实时处理。

[Slow inference protection and fault-injection validation](docs/inference-recovery.md)

[Technical specification, resume and engineering retrospectives](docs/interview/README.md)
