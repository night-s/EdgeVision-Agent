#pragma once
#include "EventLoop.h"
#include <cerrno>
#include <functional>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>
class TcpServer {
  public:
    using ConnectionId = uint64_t;
    using MessageCallback = std::function<void(ConnectionId, const std::string &)>;
    explicit TcpServer(EventLoop &loop) : loop_(loop) {}
    ~TcpServer() {
        stop();
    }
    TcpServer(const TcpServer &) = delete;
    TcpServer &operator=(const TcpServer &) = delete;
    void setMessageCallback(MessageCallback cb) {
        message_ = std::move(cb);
    }
    bool start(uint16_t port) {
        listen_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (listen_ < 0)
            return false;
        int one = 1;
        setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);
        if (bind(listen_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0 ||
            listen(listen_, 16) < 0) {
            ::close(listen_);
            listen_ = -1;
            return false;
        }
        return loop_.addFd(listen_, EPOLLIN, [this](uint32_t) { acceptClients(); });
    }
    void stop() {
        while (!clients_.empty())
            closeClient(clients_.begin()->first);
        if (listen_ >= 0) {
            loop_.removeFd(listen_);
            ::close(listen_);
            listen_ = -1;
        }
    }
    size_t connectionCount() const {
        return clients_.size();
    }
    void retain(ConnectionId id) {
        auto it = ids_.find(id);
        if (it != ids_.end())
            ++clients_.at(it->second).pending;
    }
    void release(ConnectionId id) {
        auto it = ids_.find(id);
        if (it == ids_.end())
            return;
        auto &c = clients_.at(it->second);
        if (c.pending)
            --c.pending;
        flush(it->second);
    }
    bool sendResponse(ConnectionId id, const std::string &data) {
        auto found = ids_.find(id);
        if (found == ids_.end())
            return false;
        int fd = found->second;
        auto &c = clients_.at(fd);
        if (c.out.size() + data.size() > 65536) {
            closeClient(fd);
            return false;
        }
        c.out += data;
        flush(fd);
        return ids_.count(id) > 0;
    }

  private:
    struct Client {
        ConnectionId id;
        std::string in, out;
        bool eof = false, reading = false;
        unsigned pending = 0;
    };
    EventLoop &loop_;
    int listen_ = -1;
    ConnectionId next_ = 1;
    std::unordered_map<int, Client> clients_;
    std::unordered_map<ConnectionId, int> ids_;
    MessageCallback message_;
    void closeClient(int fd) {
        auto it = clients_.find(fd);
        if (it == clients_.end())
            return;
        ids_.erase(it->second.id);
        loop_.removeFd(fd);
        ::close(fd);
        clients_.erase(it);
    }
    void acceptClients() {
        for (int i = 0; i < 32; ++i) {
            int fd = accept4(listen_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            if (clients_.size() >= 16) {
                ::close(fd);
                continue;
            }
            auto id = next_++;
            clients_.emplace(fd, Client{id, {}, {}, false});
            ids_[id] = fd;
            if (!loop_.addFd(fd, EPOLLIN | EPOLLRDHUP, [this, fd](uint32_t events) {
                    if (events & EPOLLERR) {
                        closeClient(fd);
                        return;
                    }
                    if (events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP))
                        readClient(fd);
                    if (events & EPOLLOUT)
                        flush(fd);
                }))
                closeClient(fd);
        }
    }
    void flush(int fd) {
        auto it = clients_.find(fd);
        if (it == clients_.end())
            return;
        auto &c = it->second;
        if (!c.out.empty()) {
            ssize_t n = send(fd, c.out.data(), c.out.size(), MSG_NOSIGNAL);
            if (n > 0)
                c.out.erase(0, size_t(n));
            else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                closeClient(fd);
                return;
            }
        }
        if (c.eof && !c.reading && c.pending == 0 && c.out.empty()) {
            closeClient(fd);
            return;
        }
        loop_.updateFd(fd, (c.eof ? 0u : uint32_t(EPOLLIN | EPOLLRDHUP)) |
                               (c.out.empty() ? 0u : uint32_t(EPOLLOUT)));
    }
    void readClient(int fd) {
        auto it = clients_.find(fd);
        if (it == clients_.end())
            return;
        it->second.reading = true;
        char buf[4096];
        for (int i = 0; i < 4; ++i) {
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n > 0) {
                it->second.in.append(buf, size_t(n));
                if (it->second.in.size() > 65536) {
                    closeClient(fd);
                    return;
                }
            } else if (n == 0) {
                it->second.eof = true;
                break;
            } else {
                if (errno == EINTR)
                    continue;
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    closeClient(fd);
                    return;
                }
                break;
            }
        }
        // Re-acquire after callbacks: a send failure may erase the connection.
        while ((it = clients_.find(fd)) != clients_.end()) {
            auto pos = it->second.in.find('\n');
            if (pos == std::string::npos) {
                if (it->second.in.size() > 8192) {
                    closeClient(fd);
                    return;
                }
                break;
            }
            if (pos > 8192) {
                closeClient(fd);
                return;
            }
            auto line = it->second.in.substr(0, pos);
            it->second.in.erase(0, pos + 1);
            auto id = it->second.id;
            if (message_ && !line.empty())
                message_(id, line);
        }
        it = clients_.find(fd);
        if (it != clients_.end())
            it->second.reading = false;
        flush(fd);
    }
};
