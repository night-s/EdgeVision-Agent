# 摄像头帧率调查与拔插记录
## 当前可确认的结论
2026-09-12，在相同摄像头、640×480 YUYV、请求 30 FPS、未运行推理和编码的情况下，独立 v4l2-ctl 采集自动曝光约 19～20 FPS，改为固定曝光后约 30 FPS。驱动始终报告协商 30 FPS。因此当前约 20 FPS 现象与曝光模式有关，不能归因于应用推理过慢。

摄像头为 icSpring UVC，USB 实际 480 Mb/s，枚举支持 640×480 YUYV@30。理论裸像素负载为 640×480×2×30=18,432,000 字节/秒（约 147.46 Mb/s），未计 USB 协议开销；不能只据总线名义速率证明没有带宽问题。

| 模式 | 控制值 | 180 帧对照观察 |
| --- | --- | --- |
| 自动曝光 | exposure_auto=3 | v4l2-ctl 累计估计从 15 收敛到 19.38 FPS，启动和瞬态影响明显 |
| 固定曝光 | exposure_auto=1, exposure_absolute=100 | 约 30.01～30.05 FPS |
| 固定曝光 | exposure_auto=1, exposure_absolute=300 | 约 30 FPS，但有驱动序号缺口，不能称零丢帧 |

曝光绝对值按 100 微秒为单位，100 对应 10 ms，300 对应 30 ms，参见 [Linux V4L2 Camera Controls](https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/ext-ctrls-camera.html)。测试结束按先手动、再恢复绝对值、最后恢复自动模式的顺序恢复到 auto=3 / absolute=313，并回读确认。首次批量恢复被驱动拒绝，原报告保留该失败，另附成功的顺序恢复记录。

历史 8.99 FPS 验收没有记录当时光照与曝光，无法追溯确证原因；不得把本次发现直接当作历史 9 FPS 的已证根因。改变光照的独立 A/B、满负载分段对照和长时间帧率分布仍应看后续报告。

## 新增观测
camera.requested_fps 与 negotiated_fps 分开；保留请求值用于重开，不再用整数除法覆盖它。
capture_poll 为等待耗时；capture_copy 为有效行复制耗时；camera_driver_interval 使用驱动时间戳差；camera_driver_sequence_gaps 记录可识别序号缺口。原 capture_wait_and_copy 仍保留以便对照旧日志。
驱动时间戳和应用完成时间口径不同，不能不加检查直接相减当作曝光延迟。

## 复现
独占设备且确保没有 edge_agent 占用后：
~~~sh
v4l2-ctl -d /dev/video10 --all
v4l2-ctl -d /dev/video10 --list-formats-ext
lsusb -t
v4l2-ctl -d /dev/video10 --set-fmt-video=width=640,height=480,pixelformat=YUYV --set-parm=30 --stream-mmap=4 --stream-poll --stream-count=180 --stream-to=/dev/null
~~~
修改曝光前先记录原值，手动值写入必须在 manual 模式下，测试后按顺序恢复。不要将本摄像头控制值直接套到不支持该控制的其他相机。

## 物理测试已完成的部分
用户真实拔出后，原进程仍可查询 recovering，活动录像以 capture_restart / complete=false 收尾；随后五分钟定时退出。
重新启动缺设备状态的实例，用户插入后自动恢复采集和编码，by-id 仍解析到 /dev/video10。
这两段不是同一进程跨越全程的完整拔插循环，原始记录如实保留，不能写成连续拔插验收全部通过。

建议配置使用 /dev/v4l/by-id/usb-...-video-index0，实际路径通过 ls -l /dev/v4l/by-id 查询；不要把固定 video10 当成所有板卡稳定名称。多同型号无唯一序列号相机需另做设备属性匹配。

证据见 [曝光对照](interview/evidence/camera-exposure.json)、[物理拔出状态](interview/evidence/physical-unplug.json)、[插入后恢复](interview/evidence/physical-recovery.json)。

## 完整链路光照 A/B/A
保持自动曝光与推理、MPP 编码、PC RTSP 解码同时开启，按累计计数计算：
- 操作提示之前的原照明基线：20.01 FPS。
- 增亮稳定区间：30.02 FPS。
- 恢复原照明稳定区间：20.00 FPS。
推理分别约 14.28、14.64、14.42 FPS。照度没有用仪器标定，因此结论限定于该设备和场景。报告见 [光照对照](interview/evidence/lighting-ab-a.json)，包含区间、配置、版本和计算口径。
