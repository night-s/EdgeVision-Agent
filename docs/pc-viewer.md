# PC 视频与检测演示
客户端为 tools/pc_viewer.py，支持 Windows/Linux。板端运行 edge_agent，PC 与板端网络互通。

## 安装和启动（Windows PowerShell）
先把仓库克隆到 PC 本地可写目录，再在仓库根目录执行：
~~~powershell
py -3.12 -m venv .venv-pc
.\.venv-pc\Scripts\python.exe -m pip install -r tools/requirements-pc.txt
.\.venv-pc\Scripts\python.exe tools/pc_viewer.py --host 192.168.94.126
~~~
依赖固定为 OpenCV 4.10.0.84 / NumPy 1.26.4；使用 OpenCV 的 FFmpeg 后端解码 RTSP/TCP，不要求另外安装 ffplay。若网络包索引不可达，可选择可信 HTTPS 镜像，不要关闭证书校验。

板端项目根目录：
~~~sh
./build-refactor/edge_agent configs/default.json
~~~
默认控制端口 9000，RTSP 8554 /live。若测试配置更换端口，PC 同时传 --port 与 --rtsp-port。

## 画面与同步语义
左侧是连续 RTSP 原始视频，右侧是 get_preview 返回的“同一来源帧图像 + 检测结果”，由 PC 绘制框、类别和置信度。右侧刷新目标约 4 Hz，分辨率 320×240 放大显示；帧序号和 capture_pts_ns 与检测来源相同。请求经有界 JPEG 队列异步完成，不写盘，返回体限制 60 KB。

右侧不是对左侧 RTSP 的逐帧同步叠加。当前码流没有携带应用帧 ID；将查询到的最新框直接画到任意 RTSP 帧上会错位。这一版通过同帧图像保证检测展示可追溯，下一步可做 H.264 SEI/RTP 元数据与客户端帧缓存匹配。

结果年龄包含板端响应时的年龄和 PC 收到后的等待，不包括网络在途耗时，界面标为近似值。超过 500 ms 隐藏检测框。左侧另显示 PC 收到最近视频帧距今时间，断流时不会把冻结画面伪装成新画面。

面板显示 Capture/Process/Encode FPS、检测链 P95、推理健康状态、丢弃推理帧数、录像状态。Q/Esc 退出；R 触发录像；S 抓拍，文件保存在板端。

## 单纯播放
VLC 打开 rtsp://192.168.94.126:8554/live 即可。也可以：
~~~powershell
.\.venv-pc\Scripts\python.exe tools/pc_viewer.py --source rtsp://192.168.94.126:8554/live
.\.venv-pc\Scripts\python.exe tools/pc_viewer.py --source C:\videos\event.h264
~~~
--source 模式只播放，不查询或叠加在线检测，避免把当前摄像头结果套到历史录像。文件 EOF 自动退出；H.264 裸流不能保证原始变帧率时间还原。

## 一小时 PC 解码验收
保持板端真实摄像头、推理、编码与 RTSP 运行：
~~~powershell
.\.venv-pc\Scripts\python.exe tools/pc_viewer.py --host 192.168.94.126 --headless --seconds 3600 --reconnect-every 300 --reconnect-count 10 --record-every 600 --output pc-output/soak
~~~
headless 仍在 PC 解码每一帧，只是不创建显示窗口。每 5 秒保存 PC 帧计数与板端 metrics；计划断开重连 10 次，周期触发录像。检查 report.json 与 pc-metrics.jsonl，再停止板端、验证录像解码和退出；运行命令本身不等于验收通过。

异常时先看：板端 capture_state/video_healthy；PC video_error/control_error；端口是否一致；帧计数是否继续增加。不要同时打开两个占用同一摄像头的 edge_agent。
