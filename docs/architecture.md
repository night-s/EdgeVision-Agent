# 架构与设计取舍

数据路径：V4L2 -> 帧池 -> 推理队列 / 视频队列 / JPEG任务。
推理：CPU或RGA -> RKNN -> 后处理 -> 最新检测 / ROI事件。
视频：YUYV -> videoconvert NV12 -> MPP H264 -> RTSP / 录像队列。
网络由 epoll 驱动；eventfd 用于异步任务完成投递；timerfd 周期输出指标。

显式工作线程包括采集、推理、JPEG、网络、视频投递、录像、GLib RTSP。
GStreamer、RKNN/OpenCV 内部还可能有线程，不以“总共三线程”描述系统。

## 缓冲

检查摄像头协商尺寸、stride、bytesused；拷贝有效行至 packed YUYV 后立即 QBUF。
shared_ptr<const Frame> 只读发布，释放器持有池 State，最后引用释放后归还。
像素缓冲固定预分配；共享指针控制块与第三方库仍可能分配，不是零分配/全链路零拷贝。
推理/视频队列丢旧帧；JPEG 队列满拒绝新任务；录像队列满标记缺口并从关键帧重建。
池耗尽会计数并退让，不无限扩容。stop/start/restart 代际号隔离在途旧结果。

## 模型

校验 640×640、三个 NCHW 输出头；当前模型是 logits。
共享 Letterbox 几何逆变换与 anchor 解码；NMS 有候选/结果上限。
原生 affine INT8/UINT8 逐标量反量化，避免 SDK 全量展开 float；不支持类型回退浮点。
固定输入对照检查类别、框、置信度，但不能代替 mAP 评估。

RGA 使用同步虚拟地址包装，尚无 DMA-BUF 共享。640×480 下未证明收益，默认 CPU。

## 视频与录像

使用 GStreamer/MPP 插件，不是自研编码器。板端 MPP YUY2 输入发生内部 RGA 错误，
因此显式转 NV12。预览不等待检测；RTSP 从关键帧开始，appsrc 缓冲超限等待新关键帧。
第三方组件/OS 仍有缓冲，弱网、多客户端公平性需额外测量。

ROI 使用框中心和连续帧确认，无跟踪 ID；尾随时间内再触发延长同一事件。
前缓存按 GOP 保留，可能多出一个 GOP。Annex-B H264 + JSON 记录起止时间和收尾原因；
裸流不保留逐帧容器 PTS，不承诺变帧率精确回放或音画同步。
已增加事件容量、数量、空间和时长约束，保留活跃及不完整事件；尚无 MP4/MKV 封装。写盘失败不阻断推理，但永久阻塞的磁盘调用
无法由本进程保证硬截止退出。

## 异常与退出

O_NONBLOCK + poll(200ms)，连续 5 次失败重开摄像头；缺失时固定间隔重试，
网络查询继续。持续恢复而非无限忙轮询，重试次数可观察。
连续 3 次推理异常暂停，可 start 重试；尚不自动重建 RKNN 上下文。
视频异常标记 video_healthy=false，尚不自动重建编码器。

退出停止投递、唤醒队列、join采集/推理、排空JPEG、编码EOS限时等待、
排空录像、停止网络与释放fd。厂商驱动内核级卡死不在用户态取消能力内。

旧 SkillManager、MemoryPool、SafeQueue、ThreadPool 和关联 Skill 实现已删除；当前仅保留 app/VisionPipeline 主链路。
