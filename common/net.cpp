#include "common/net.h"

#include <unistd.h>
#include <sys/socket.h>
#include <cerrno>
#include <csignal>

namespace chatlab {

    // 忽略 SIGPIPE 信号，防止往已关闭的连接写数据时进程被杀死
    void ignore_sigpipe() {
        signal(SIGPIPE, SIG_IGN);
    }

    // 循环发送，直到 len 字节全部写完或出错
    bool send_all(int fd, const void* buf, std::size_t len) {
        const char* ptr = static_cast<const char*>(buf);
        std::size_t sent = 0;

        while (sent < len) {
            // 从 ptr + sent 继续发，还需要发 len - sent 字节
            // MSG_NOISIGNAL 避免触发 SIGPIPE
            ssize_t n = send(fd, ptr + sent, len - sent, MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EINTR) {
                    continue; // 被信号打断，重试
                }
                return false; // 出现其他错误
            }
            if (n ==0 ) {
                return false; // 对端关闭连接
            }
            sent += static_cast<std::size_t>(n);
        }
        return true;
    }

    // 循环接收，直到 len 字节全部接完或出错
    bool recv_exact(int fd, void* buf, std::size_t len) {
        char* ptr = static_cast<char*>(buf);
        std::size_t received = 0;

        while (received < len) {
            // 从 ptr + received 继续接，还需要接 len - received 字节 
            ssize_t n = recv(fd, ptr + received, len - received, 0);
            if (n < 0) {
                if (errno == EINTR) {
                    continue; // 被信号打断，重试
                }
                return false; // 出现其他错误
            }
            if (n == 0) {
                return false; // 对端关闭连接
            }
            received += static_cast<std::size_t>(n);
        }
        return true;
    }

    // 实现 RAII 封装 fd 的析构、移动、release、reset

    FdGuard::~FdGuard() noexcept { // 析构
        // fd_ >= 0 才关闭，避免无效关闭
        if (fd_ >= 0) {
            close(fd_);
        }
    }

    FdGuard::FdGuard(FdGuard&& other) noexcept : fd_(other.fd_) { // 移动构造
        other.fd_ = -1;
    }

    FdGuard& FdGuard::operator=(FdGuard&& other) noexcept { // 移动赋值
        if (this != &other) {
            if (fd_ >= 0) {
                close(fd_);
            }
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    int FdGuard::release() noexcept { // 放弃 fd 的“所有权”，返回裸 fd
        int old = fd_;
        fd_ = -1;
        return old;
    }

    void FdGuard::reset(int new_fd) noexcept { // 关闭旧 fd，接管新 fd
        if (fd_ >= 0 && fd_ != new_fd){
            close(fd_);
        }
        fd_ = new_fd;
    }

}  