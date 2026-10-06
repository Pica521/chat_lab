#pragma once

#include <cstddef>

namespace chatlab {

    /// 循环发送 len 字节，直到全部写完或出错
    /// 返回 true 表示全部成功；false 表示对端关闭或出错
    bool send_all(int fd, const void* buf, std::size_t len);

    /// 循环接收 len 字节，直到收满或出错
    /// 返回 true 表示收满；false 表示对端关闭或出错
    bool recv_exact(int fd, void* buf, std::size_t len);

    /// RAII 封装文件描述符，析构时自动 close
    class FdGuard {
        public:
            FdGuard() noexcept : fd_(-1) {}
            explicit FdGuard(int fd) noexcept : fd_(fd) {}
            ~FdGuard() noexcept;

            // 禁止拷贝：fd 是独占资源
            FdGuard(const FdGuard&) = delete;
            FdGuard& operator=(const FdGuard&) = delete;

            // 允许移动(转移fd的"所有权")
            FdGuard(FdGuard&& other) noexcept;
            FdGuard& operator=(FdGuard&& other) noexcept;

            int get() const noexcept { return fd_; }
            bool valid() const noexcept { return fd_ >= 0; }

            // 放弃所有权，返回裸 fd，不再负责关闭
            int release() noexcept;

            // 关闭当前 fd，接管新 fd
            void reset(int new_fd = -1) noexcept;

        private:
            int fd_;
    };

    /// 忽略 SIGPIPE 信号，应在程序启动时调用一次
    void ignore_sigpipe();
} // namespace chatlab