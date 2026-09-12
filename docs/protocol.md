# 控制协议

TCP UTF-8 JSON 每行一个对象；请求上限8192字节，连接输出上限65536字节，
最多16个连接。超长输入或慢消费者超限会断开。无认证，仅用于受信任网络。

~~~json
{"id":1,"cmd":"get_status"}
{"id":2,"cmd":"set_threshold","value":0.45}
{"id":3,"cmd":"capture"}
~~~

| cmd | 行为 |
|---|---|
| get_status | 摄像头/推理/视频状态与错误、池与队列 |
| get_metrics | 计数、近窗耗时、速率、CPU、RSS |
| get_detections | 最近结果、帧序号、对应帧年龄 |
| start / stop | 启停推理，禁止旧代际结果发布，不停止视频 |
| set_threshold | 修改阈值，value在(0,1) |
| capture | 最近两秒内有效帧，无条件抓拍 |
| record_event | 请求/延长录像 |
| restart_camera | 请求采集线程重开设备 |

同步成功返回 status=completed、code=0。capture 先 accepted，再 completed/error。
record_event/restart_camera 返回 accepted，最终结果分别查清单/状态。
400为无效请求或当前条件不允许，429为抓拍队列满，500为抓拍写盘失败。
文件路径在板端，不传输内容。兼容 skill_name=CaptureSkill/QuerySkill。

id 为整数/字符串，JSON表示不超过128字节。同连接缓存最近128个响应，重复id即使参数
不同也回放旧响应；异步完成替换accepted；id=null不去重。不是跨连接或持久化exactly-once。
TCP写半关闭仍能收到已接受抓拍的最终响应，完全断连不会误发给复用fd的新连接。

metrics.jsonl 每5秒输出；速率查询复用至少约一秒窗口，rate_window_s显示实际长度。
stages_ms.count是保留样本数，不是累计数；CPU以单核心100%为口径。

See [inference recovery](inference-recovery.md) for get_events, set_infer_delay, inference_health and detection fresh semantics.
