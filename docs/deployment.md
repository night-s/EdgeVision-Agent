# 构建与部署
板端已知环境：RK3568、Linux 5.10.160 aarch64、OpenCV 4.2.0、GStreamer 1.18.5、MPP 插件 1.14.4；详细版本见 validation.md。RKNN runtime 与模型必须匹配，依赖包版本不等于运行库版本。

## 原生构建
板端 CMake 3.16.3 不支持 presets；使用：
~~~sh
cmake -S . -B build-refactor -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/edgevision
cmake --build build-refactor -j2
(cd build-refactor && ctest --output-on-failure)
~~~
CMake >=3.20 可用 cmake --preset board-release 和 cmake --build --preset board-release。host-tests 预设关闭 RK3568 主程序，但需要 OpenCV 和 Linux 系统调用环境；Windows 不能直接跑这些 Linux C++ 测试。

## 格式检查
使用 clang-format 18.1.8 和根目录 .clang-format。只格式化 app/core/hardware/media/tests 的自有 C++ 及 main.cpp，不格式化 third_party。不要用 shell 的 ** 展开假设所有环境都支持递归通配。

## 交叉编译
~~~sh
export EDGE_SYSROOT=/absolute/path/to/rk3568-sysroot
cmake -S . -B build-cross -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-gnu.cmake -DCMAKE_INSTALL_PREFIX=/opt/edgevision -DEDGE_DEPS=/nonexistent
cmake --build build-cross -j2
~~~
模板要求 aarch64-linux-gnu-g++、匹配 sysroot 内的 OpenCV/RKNN/GStreamer/MPP/RGA 开发依赖。模板已提供，但没有匹配 PC sysroot 的实际交叉构建证据；不要宣称已完成交叉编译验收。

## 安装布局
先进行不影响系统的 staging 验证：
~~~sh
DESTDIR="$PWD/output/install-stage" cmake --install build-refactor
~~~
程序 /opt/edgevision/bin/edge_agent；RKNN 和 RTSP server 运行库 /opt/edgevision/lib；模型与示例配置 /opt/edgevision/share/edgevision；运行配置 /etc/edgevision/config.json；可写状态 /var/lib/edgevision。
除打包的 RTSP server 库外，MPP/GStreamer 等平台依赖仍由板端系统提供；本安装规则不打包整个根文件系统。

安装服务前创建 edgevision 用户、组与数据目录，将示例配置复制到 /etc/edgevision/config.json，修改 device 为实际 /dev/v4l/by-id/...-video-index0。服务模板在 deploy/edgevision.service，不会自动启用。
检查 video 组、/dev/rknpu、/dev/dri、/dev/mpp_service、/dev/rga 等实际设备访问权限；不同 BSP 节点不同，需要按目标板配置 udev/附加组。不要用 chmod 777 或直接声称此模板跨板即装即用。

确认前台运行和权限后，由管理员安装服务文件：
~~~sh
systemctl daemon-reload
systemctl enable --now edgevision
systemctl status edgevision
journalctl -u edgevision
systemctl stop edgevision
~~~
模板依赖 /opt/edgevision 固定安装路径。Restart=on-failure 处理进程退出，不是 RKNN 调用内部卡死恢复。TimeoutStopSec 到期系统可终止进程，可能留下不完整事件；故障证据需保留。

## 存储策略
默认录像预算 256 MiB、最多 128 个事件文件、至少 64 MiB 剩余空间、单段最长 60 秒，可在配置修改 record_max_mb / record_max_files / record_min_free_mb / record_max_seconds。

配额每秒检查；允许约一秒编码量和当前包/清单的临时超额，不是严格逐字节硬限额。只删除名称、格式清单均符合本项目且 complete/write_ok 为真的结束事件；活跃文件、不完整文件、符号链接和无关文件不删除。保护文件占满预算时拒绝新录像并报告 storage_budget，不无限扩大存储。

最长时长按文件打开后的单调时间限制，不含启动前预录时长；完整段以 max_duration 收尾。持续触发时后续可从关键帧开始新段；不是严格无缝拼接。磁盘调用永久阻塞仍不能保证硬截止，JPEG 抓拍和 metrics 日志暂不包含在事件配额内。

安装验收同时执行 staging 程序的 --help，确认动态库可加载；不能只看文件已复制。已修复原规则遗漏 RTSP server 库导致的启动失败。
