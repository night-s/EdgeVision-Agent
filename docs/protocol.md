# 控制协议
TCP UTF-8 JSON，每行一个对象。默认 9000；请求最大 8192 字节，单连接待发最大 65536 字节，最多 16 个连接。无认证/TLS，限受信任局域网。

~~~json
{"id":1,"cmd":"get_status"}
{"id":2,"cmd":"set_threshold","value":0.45}
{"id":3,"cmd":"capture"}
~~~

| cmd | 行为 |
| --- | --- |
| get_status | 摄像头请求/协商帧率、健康、队列和录像状态 |
| get_metrics | 计数、耗时、CPU/RSS/FD |
| get_detections | 帧序号、年龄、fresh；过期不返回检测框 |
| get_preview | 异步返回同一来源帧 JPEG 与检测结果 |
| get_events | 当前录像及最近 32 条结束事件 |
| start / stop | 启停推理，旧代际结果失效，不停止视频 |
| set_threshold | value 在 (0,1) |
| capture | 异步抓拍最近两秒内的有效帧，返回板端路径 |
| record_event | 请求/延长事件，重叠触发会合并 |
| restart_camera | 请求重开设备 |
| set_infer_delay | 仅 allow_fault_injection=true 可用，整数 value 为 0～5000 ms |

同步成功 completed/code=0。capture/get_preview 先 accepted，再 completed/error。record_event/restart_camera 返回 accepted，需轮询最终状态。400 是前置条件/参数错误，429 是队列满，500 是异步任务失败。

get_preview 包含 sequence、capture_pts_ns、age_ms、jpeg_hex、图像/源尺寸和 detections。JPEG 320×240、质量 55，最大 24000 字节，总响应最大 60000 字节。图像与检测来自同一不可变帧引用，框坐标以 source_width/source_height 为基准。capture_pts_ns 相对服务单调时钟原点，不是 UTC；RTSP 不携带该序号，不能据此声称逐帧同步叠加。

id 为整数/字符串，JSON 表示不超过 128 字节；一般命令同连接缓存最近 128 个响应，重复 id 回放旧响应。get_preview 不缓存大图响应；客户端应使用新 id。id=null 不去重，非跨连接持久化 exactly-once。读半关闭仍可接收已接受任务结果，完全断开不会向复用 fd 误发。

metrics.jsonl 每 5 秒输出，查询速率至少约一秒窗口。stages_ms 每阶段最新 1024 个样本，count 不是累计数。CPU 单核 100% 口径；fd_count 包含遍历目录时的 FD。processed 包含完成但结果过期的帧，detections_published 为实际发布数。

inference_health.fast_streak 是连续快处理次数；恢复次数看 inference_recoveries。camera_driver_sequence_gaps、inference_dropped、video_dropped 为不同链路口径。
