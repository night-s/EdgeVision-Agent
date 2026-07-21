#ifndef TCP_SERVER_H
#define TCP_SERVER_H

#include "EventLoop.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <string>
#include <unordered_map>
#include <functional>
#include <iostream>
#include <cstring>
#include <cerrno>

/**
 * TcpServer — Reactor 驱动的非阻塞 TCP 服务
 *
 * 设计要点：
 *  - 非阻塞 listen socket (SOCK_NONBLOCK)
 *  - Level-triggered epoll 模式
 *  - 新行分隔 (\n) 的 JSON RPC 消息协议
 *  - 写缓冲 + EPOLLOUT 驱动发送
 *  - 每个连接独立读写缓冲
 */
class TcpServer {
public:
    using MessageCallback    = std::function<void(int client_fd, const std::string& msg)>;
    using ConnectionCallback = std::function<void(int client_fd)>;
    using CloseCallback      = std::function<void(int client_fd)>;

    static constexpr size_t kReadBufSize = 8192;

    explicit TcpServer(EventLoop& loop)
        : loop_(loop)
        , listen_fd_(-1)
        , started_(false)
    {}

    ~TcpServer() {
        if (started_) stop();
    }

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    // ========== 生命周期 ==========

    /// 启动监听, 注册到 EventLoop
    bool start(uint16_t port) {
        if (started_) return true;

        // 创建非阻塞 listen socket
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (listen_fd_ < 0) {
            std::cerr << "[TcpServer] socket() failed: " << std::strerror(errno) << std::endl;
            return false;
        }

        int opt = 1;
        if (::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            std::cerr << "[TcpServer] setsockopt(REUSEADDR) failed: " << std::strerror(errno) << std::endl;
            ::close(listen_fd_);
            return false;
        }

        struct sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port        = htons(port);

        if (::bind(listen_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "[TcpServer] bind() port " << port << " failed: "
                      << std::strerror(errno) << std::endl;
            ::close(listen_fd_);
            return false;
        }

        if (::listen(listen_fd_, SOMAXCONN) < 0) {
            std::cerr << "[TcpServer] listen() failed: " << std::strerror(errno) << std::endl;
            ::close(listen_fd_);
            return false;
        }

        // 注册 listen_fd 到 EventLoop
        if (!loop_.addFd(listen_fd_, EPOLLIN,
                         [this](uint32_t revents) { onAccept(revents); }))
        {
            ::close(listen_fd_);
            return false;
        }

        started_ = true;
        std::cout << "[TcpServer] Listening on port " << port << std::endl;
        return true;
    }

    /// 停止服务：关闭所有连接，注销 listen fd
    void stop() {
        if (!started_) return;
        started_ = false;

        // 注销并关闭所有客户端连接
        auto conns = std::move(connections_);
        for (auto& [fd, _] : conns) {
            loop_.removeFd(fd);
            ::close(fd);
        }

        // 注销并关闭 listen fd
        loop_.removeFd(listen_fd_);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }

    /// 当前已连接数
    size_t connectionCount() const { return connections_.size(); }

    // ========== 回调注册 ==========

    void setMessageCallback(MessageCallback cb)    { message_cb_ = std::move(cb); }
    void setConnectionCallback(ConnectionCallback cb) { conn_cb_ = std::move(cb); }
    void setCloseCallback(CloseCallback cb)        { close_cb_ = std::move(cb); }

    // ========== 发送 ==========

    /// 向指定客户端发送响应（写缓冲 + EPOLLOUT 驱动）
    bool sendResponse(int client_fd, const std::string& response) {
        auto it = connections_.find(client_fd);
        if (it == connections_.end()) return false;

        auto& conn = it->second;

        if (!conn.write_buf.empty()) {
            // 已有待发送数据，追加到缓冲区
            conn.write_buf.append(response);
            return true;
        }

        // 尝试直接发送
        ssize_t n = ::write(client_fd, response.data(), response.size());
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 缓冲区满，全部缓冲
                conn.write_buf = response;
                enableWrite(client_fd);
                return true;
            }
            // 连接错误
            onClose(client_fd);
            return false;
        }

        if (static_cast<size_t>(n) < response.size()) {
            // 部分发送，缓冲剩余部分
            conn.write_buf = response.substr(n);
            enableWrite(client_fd);
        }

        return true;
    }

    /// 向所有已连接的客户端广播消息
    void broadcast(const std::string& response) {
        for (auto& [fd, _] : connections_) {
            sendResponse(fd, response);
        }
    }

private:
    struct TcpConnection {
        std::string read_buf;   // 接收缓冲区
        std::string write_buf;  // 发送缓冲区
    };

    // ========== 连接管理 ==========

    /// 接受新连接
    void onAccept(uint32_t revents) {
        if (revents & (EPOLLERR | EPOLLHUP)) {
            std::cerr << "[TcpServer] listen fd error" << std::endl;
            return;
        }

        while (true) {
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);

            int client_fd = ::accept4(listen_fd_, (struct sockaddr*)&client_addr,
                                      &addr_len, SOCK_NONBLOCK);
            if (client_fd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;  // 所有待处理连接已接受完毕
                }
                if (errno == EINTR) continue;
                std::cerr << "[TcpServer] accept4() failed: "
                          << std::strerror(errno) << std::endl;
                break;
            }

            // 注册客户端 fd 到 EventLoop
            connections_[client_fd] = TcpConnection{};

            if (!loop_.addFd(client_fd, EPOLLIN | EPOLLRDHUP,
                             [this, client_fd](uint32_t revents) {
                                 onClientEvent(client_fd, revents);
                             }))
            {
                // 注册失败，关闭连接
                connections_.erase(client_fd);
                ::close(client_fd);
                continue;
            }

            // 连接建立回调
            if (conn_cb_) {
                conn_cb_(client_fd);
            }
        }
    }

    /// 客户端 fd 事件分发
    void onClientEvent(int client_fd, uint32_t revents) {
        if (revents & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
            onClose(client_fd);
            return;
        }

        if (revents & EPOLLIN) {
            onReadable(client_fd);
        }

        if (revents & EPOLLOUT) {
            onWritable(client_fd);
        }
    }

    /// 可读事件处理
    void onReadable(int client_fd) {
        auto it = connections_.find(client_fd);
        if (it == connections_.end()) return;

        auto& conn = it->second;
        char buf[kReadBufSize];

        while (true) {
            ssize_t n = ::read(client_fd, buf, sizeof(buf));
            if (n > 0) {
                conn.read_buf.append(buf, static_cast<size_t>(n));
            } else if (n == 0) {
                // 对端关闭连接
                onClose(client_fd);
                return;
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;  // 数据已读完
                }
                // 读错误
                onClose(client_fd);
                return;
            }
        }

        // 提取完整的新行分隔消息
        while (true) {
            auto pos = conn.read_buf.find('\n');
            if (pos == std::string::npos) break;

            std::string msg = conn.read_buf.substr(0, pos);
            conn.read_buf.erase(0, pos + 1);

            // 去掉末尾空白
            while (!msg.empty() && (msg.back() == '\r' || msg.back() == ' ' || msg.back() == '\t')) {
                msg.pop_back();
            }

            if (!msg.empty() && message_cb_) {
                message_cb_(client_fd, msg);
            }
        }
    }

    /// 可写事件处理 — 发送写缓冲中的数据
    void onWritable(int client_fd) {
        auto it = connections_.find(client_fd);
        if (it == connections_.end()) return;

        auto& conn = it->second;
        if (conn.write_buf.empty()) return;

        ssize_t n = ::write(client_fd, conn.write_buf.data(), conn.write_buf.size());
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;  // 下次再发
            }
            onClose(client_fd);
            return;
        }

        if (static_cast<size_t>(n) < conn.write_buf.size()) {
            conn.write_buf.erase(0, static_cast<size_t>(n));
        } else {
            conn.write_buf.clear();
            // 数据全部发送完毕，注销 EPOLLOUT 事件
            disableWrite(client_fd);
        }
    }

    /// 关闭连接
    void onClose(int client_fd) {
        loop_.removeFd(client_fd);
        ::close(client_fd);
        connections_.erase(client_fd);

        if (close_cb_) {
            close_cb_(client_fd);
        }
    }

    // ========== 辅助 ==========

    void enableWrite(int fd) {
        auto it = loop_.updateFd(fd, EPOLLIN | EPOLLRDHUP | EPOLLOUT);
        (void)it;  // 忽略返回值
    }

    void disableWrite(int fd) {
        auto it = loop_.updateFd(fd, EPOLLIN | EPOLLRDHUP);
        (void)it;
    }

    EventLoop& loop_;
    int listen_fd_;
    bool started_;

    std::unordered_map<int, TcpConnection> connections_;

    MessageCallback    message_cb_;
    ConnectionCallback conn_cb_;
    CloseCallback      close_cb_;
};

#endif // TCP_SERVER_H
