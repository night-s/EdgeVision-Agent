# 慢推理保护与验证

消费者每次取队列最新帧，旧帧丢弃计入 inference_dropped。输入和结果有效期默认 500 ms；过期结果不发布、不触发录像。get_detections 增加 fresh，过期后 detections 为空，保留 sequence/age_ms 供排障。

服务耗时 inference_service 包含预处理、RKNN 输入/执行/输出、后处理与测试延迟，不包括等待输入和排队。连续 3 次耗时 >= infer_slow_ms（默认 150 ms）进入 degraded，每次处理后冷却 infer_degraded_interval_ms（默认 200 ms）。连续 10 次低于慢阈值 75% 才恢复 normal；中间区间会打断连续计数。阈值应按同模型、同输入、同负载的基线调整。

get_status 与周期 metrics 的 inference_health 包含状态、active_ms、last_service_ms 及连续计数。活跃调用超过 infer_stall_ms（默认 2000 ms）报告 stalled。无输入造成的低 FPS 不会被误判为 NPU 慢。processed/process_fps 包含处理完成但结果过期的帧，不能视作有效发布率。

| 现象 | 排查与处理 |
| --- | --- |
| queue_wait 高，inference 正常 | 处理跟不上输入；最新帧策略限制积压 |
| preprocess / postprocess 高 | 查看 CPU 竞争、格式转换与输出规模；固定输入对照 |
| inference 单独升高 | 核对 NPU 并发、温度、频率和驱动日志；降低提交速率观察恢复 |
| active_ms 持续增长 | 活跃任务未返回，结合线程栈定位具体阶段 |
| capture_fps 同时下降 | 先检查采集超时和设备状态，再检查推理 |

active_ms 是整个处理任务年龄，不能单独证明卡在 rknn_run。模拟延迟验证调度和控制响应，不等价于真实驱动故障。

## 恢复边界

本实现不会跨线程强杀 RKNN，也不会并发销毁其上下文。真实调用永久阻塞时，退出 join 仍可能等待。下一步的有界故障恢复应把推理移到独立子进程，由父进程负责超时、重启、帧缓冲回收及代际隔离。start 只是恢复开关，不重建上下文。

录像线程每 100 ms 检查输入，2 秒无包则关闭活动文件并写 input_timeout / complete=false。摄像头重启设置时间边界并清空预录缓存，边界前编码包丢弃。get_events 返回 current 和最近 32 条结束事件，含 event_id、path、reason、complete、write_ok、manifest_ok。

ID 在实际打开文件时产生。record_event 为可合并触发，不是逐请求独立录像；无包且尚未打开文件的请求没有独立超时终态。历史仅驻留内存，磁盘清单保留。本轮尚未实现磁盘配额、连续事件时长限制、MP4/MKV、编码器自动重建与推理进程隔离。

## 验证

项目根目录执行：

~~~sh
cmake --build build-refactor -j2
(cd build-refactor && ctest --output-on-failure)
python3 tests/inference_health_validation.py
~~~

测试使用合成输入、真实 RKNN/MPP、TCP 19300 和 RTSP 18560，仅停止自己启动的进程。仍共享 NPU 与编码硬件，性能验收需避免并发负载。报告写入 output/health-validation-*。

流程：正常 → 注入 600 ms → 降级与过期过滤 → 注入 1500 ms → 观察 stalled → 延迟归零 → 恢复。检查期间采集编码持续、截图响应、录像完成状态与退出。

allow_fault_injection 默认 false。仅测试配置设为 true 后可调用 set_infer_delay，value 为 0～5000 的整数毫秒；该延迟插入 RKNN 返回之后。正式配置不要开启该接口。
