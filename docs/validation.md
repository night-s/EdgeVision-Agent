# 验收与测量

原始完整日志和媒体在 output/，不纳入 Git；公开报告及配置已归档至 interview/evidence。下方保留各阶段实测条件，最新 P0 结果见文末。

~~~bash
(cd build-refactor && ctest --output-on-failure)
python3 tests/board_validation.py
python3 tools/benchmark.py --seconds 60 --backends cpu rga
~~~

board_validation 使用19000/18554端口，独占/dev/video10，只停止自己启动的进程。
测试设备路径缺失/恢复、摄像头重开、协议边界、异步半关闭抓拍、慢控制客户端、
RTSP播放解码和重连、录像解码、慢推理视频独立性、fd与退出。
不解绑物理USB、不修改驱动；不能替代物理拔插和24小时稳定性。

板端GIO代理解析组件会使gst-launch RTSP客户端在连接时抛出C++异常；
测试子进程设 GIO_USE_PROXY_RESOLVER=dummy 后播放正常，不修改系统环境。

edge_model_validation 交替测试浮点/原生输出，预热2轮后统计10轮；
另测CPU/RGA像素差分与检测结果。使用仓库已有抓拍转换的固定帧，图像不离开板端。
检查正确性不等于完成COCO mAP，不把推理倒数作为系统FPS，
本机RTSP解码不等于外部PC播放延迟或弱网验证。

## 本次板端实测（2026-09-11 至 2026-09-12）

RK3568 / Linux 5.10.160 aarch64，约4GB内存；OpenCV 4.2.0、GStreamer 1.18.5，
MPP插件1.14.4，pkg-config报告MPP 1.3.8。RGA包版本2.1.0，库运行时日志为
rga_api version 1.8.0_[0]；不能将包版本直接当作运行库版本。

模型 SHA256：5657864d504e0479a6bbf7bad43ba7806b026d9b7d19478d173ddef1b4428fc4

### 完整流水线对照

Release 构建；同一640×480 packed YUYV帧按30Hz回放，真实NPU与MPP编码；
队列2帧、池12帧、阈值0.35、logits、视频开启。三组顺序运行，各60秒，
不与编译并行；跳过首个窗口，用内部累计计数差计算稳态速率。

| 路径 | 采集FPS | 检测FPS | 编码FPS | 预处理P95 ms | 采集完成到检测完成P95 ms |
|---|---:|---:|---:|---:|---:|
| CPU + SDK浮点输出 | 29.99 | 11.74 | 29.99 | 2.72 | 157.68 |
| CPU + 原生量化输出（默认） | 29.99 | 14.59 | 29.99 | 4.71 | 135.19 |
| RGA + 原生量化输出 | 29.99 | 14.27 | 29.99 | 7.45 | 138.01 |

本次单轮对照：原生量化输出使处理帧率提高约24.3%，检测链P95下降约14.3%。
不是NPU本体加速，也不是多次重复实验的统计结论。CPU/RGA前处理受到同步及内存搬运影响；
当前输入下RGA更慢，保留CPU默认路径。

原始报告：
- [原生输出CPU/RGA](interview/evidence/benchmark-native.json)
- [浮点输出CPU](interview/evidence/benchmark-float.json)
- 公开配置见同目录 benchmark-*-config-*.json；完整 run.log、metrics.jsonl 和输入图像仅保存在板端 output 中，未公开。

### 固定帧正确性与阶段对照

[模型对照](interview/evidence/model-validation.json) 使用仓库已有人员抓拍转换的单帧。
12轮浮点/原生输出交替执行（2轮预热，统计10轮），类别、框坐标及置信度一致。
输出获取+后处理均值：浮点20.38ms，
原生6.70ms。

模型输出验证为logits；probabilities模式明确报错，避免静默空检测。
CPU/RGA图像最大绝对差1/255，
平均绝对差0.152/255。
两者均检测到人员，但框坐标存在差异，因此不宣称CPU/RGA结果完全等价，
更不能用这一帧证明整体检测精度。

### 功能验收

[验收报告](interview/evidence/board-acceptance.json) 所列
11项全部通过，包括基础协议、异步半关闭抓拍、摄像头路径缺失/恢复、
两次摄像头重启、RTSP两次播放解码、事件录像解码、慢推理视频独立性与退出。

真实摄像头请求30FPS，但当时实采约8.99FPS；
编码约8.99FPS，人为加入150ms推理延迟后检测约
3.79FPS。该实采速率不是板卡吞吐上限，未进一步确认
摄像头曝光/驱动等限制原因。固定回放实验单独验证30FPS视频能力。

退出耗时0.216秒，退出码0。测试前后FD增加2个，
含首次媒体会话的延迟初始化，未观察到每次连接累积；这不能替代长时间泄漏验证。
基础CTest覆盖队列/帧池、Letterbox、anchor/NMS、量化解码、定时器、
停止竞态与录像排空。Release构建和git diff --check通过。

该阶段尚未完成：24小时长稳、物理拔插、磁盘耗尽、外部PC播放时延、
数据集mAP、DMA-BUF共享和MP4/MKV封装。无自动化任务留在后台运行。

## Slow-inference protection acceptance (2026-09-12)

- Release build passed; edge_core CTest passed (including latest-frame selection,
  health hysteresis/stall observation, and recorder inactivity timeout).
- tests/inference_health_validation.py passed 8 checks on synthetic 30 Hz input
  with real RKNN and MPP. Report:
  [archived report](interview/evidence/health-acceptance.json).
- Injected 600 ms delay -> degraded and expired results suppressed; injected
  1500 ms delay -> stalled observable; removing delay -> normal with fresh results.
- Capture/encoding kept advancing during slowdown; snapshot completed; recording
  completion was queryable. Camera restart closed the old event as incomplete
  with capture_restart; a subsequent event completed.
- 1 degradation, 1 recovery, 7 discarded stale results; queue_wait P95 29.90 ms
  across the mixed fault/recovery run. Shutdown 0.219 s.
- This is a policy acceptance test, not a steady-state performance benchmark,
  real NPU driver hang recovery test, or long-duration soak test.

## P0 补充验证（2026-09-13）

完整链路光照 A/B/A 对照：原光照 20.01 FPS、增亮 30.02 FPS、恢复 20.00 FPS；见 [报告](interview/evidence/lighting-ab-a.json) 与 [排查说明](camera-investigation.md)。历史 8.99 FPS 不作追溯归因。

PC 播放与长稳方法见 [客户端说明](pc-viewer.md)。自动验收脚本 tools/analyze_soak.py 检查有效观察时长、采样断档、持续解码、重连、退出、录像解码及 RSS/FD 增量；不以电脑休眠后的墙钟时长充当运行时长。当前一小时测试结果待完成后单独归档。
