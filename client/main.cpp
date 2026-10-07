#include "common/net.h"
#include "common/protocol.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <atomic>
#include <cstdlib>

namespace {

    // 客户端默认连接的服务端地址和端口。
    // host.docker.internal 是 Docker 容器访问宿主机的特殊域名。
    // 如果在虚拟机里直接跑客户端，可以改成 127.0.0.1。
    constexpr const char* DEFAULT_HOST = "host.docker.internal";
    constexpr int DEFAULT_PORT = 9000;

    // 保护 std::cout，避免主线程和接收线程同时打印导致输出交错。
    std::mutex g_cout_mutex;

    // 控制主循环是否继续运行。
    // 当接收线程检测到连接断开时，把它设为 false，让主线程也退出。
    std::atomic<bool> g_running{true};

    int connect_to_server(const std::string& host, int port) {
        // 1.getaddrinfo 提示
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;  // 允许 IPv4 和 IPv6
        hints.ai_socktype = SOCK_STREAM;

        // 2.解析 host 和 port
        std::string port_str = std::to_string(port);
        addrinfo* result = nullptr;
        int rc = ::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result);
        if (rc != 0) {
            std::cerr << "getaddrinfo failed: " << gai_strerror(rc) << std::endl;
            return -1;
        }

        // 3.RAII 管理 result
        struct AddrInfoGuard {
            addrinfo* p;
            ~AddrInfoGuard() { if (p) ::freeaddrinfo(p); }
        } guard{result};
        
        // 4.遍历候选地址，创建 socket，连接服务器
        for (addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
            // 创建 socket
            int fd = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
            if (fd < 0) {
                continue;
            }
            // 连接服务器
            if (::connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
                std::cout << "Connected to " << host << ":" << port << "(fd=" << fd << ")" << std::endl;
                return fd;
            }
            // 连接失败
            ::close(fd);
        }
        // 全部失败
        std::cerr << "Failed to connect to " << host << ":" << port << std::endl;
        return -1;
    }

}  // namespace

int main(int argc, char* argv[]) {
    // 默认值
    std::string host = DEFAULT_HOST;
    int port = DEFAULT_PORT;

    // 解析命令行参数
    if (argc >= 2) {
        host = argv[1];
    }
    if (argc >= 3) {
        port = std::atoi(argv[2]);
        if (port <= 0 || port > 65535) {
            std::cerr << "Invalid port: " << argv[2] << std::endl;
            return 1;
        }
    }

    // 连接服务端
    int fd = connect_to_server(host, port);
    if (fd < 0) {
        std::cerr << "connect failed" << std::endl;
        return 1;
    }

    std::cout << "connected, fd=" << fd << std::endl;
    ::close(fd);
    return 0;
}