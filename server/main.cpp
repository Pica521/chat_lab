#include "common/net.h"
#include "common/protocol.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <cstdlib>

namespace{

    // 默认监听端口
    constexpr int DEFAULT_PORT = 9000;
    // 内核允许排队的最大连接数
    constexpr int LISTEN_BACKLOG = 128;

    // 客户结构体
    // 每个客户端连接对应一个 ClientInfo，由 shared_ptr 管理
    // 因为同一个对象会被两个 map（按 fd、按用户名）同时引用
    struct ClientInfo {
        int fd = -1;  // socket fd
        std::string username;
        bool logged_in = false;
        std::mutex send_mutex; // 保护该连接的发送
    };

    // 全局互斥锁，保护两个 map 的所有操作
    std::mutex g_clients_mutex;
    // 按 fd 查找客户端。建立时插入，断开时删除
    std::map<int, std::shared_ptr<ClientInfo>> g_clients_by_fd;
    // 按用户名查找
    std::map<std::string, std::shared_ptr<ClientInfo>> g_clients_by_name;

    // 创建 socket，绑定端口，开启监听
    int create_listen_socket(int port) {
        // 1.创建 socket
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            std::cerr << "create socket failed: " << std::strerror(errno) << std::endl;
            return -1;
        }
        chatlab::FdGuard guard(fd);

        // 设置 SO_REUSEADDR
        int opt =1;
        if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            std::cerr << "set sockopt failed: " << std::strerror(errno) << std::endl;
            return -1;
        }

        // 3.绑定端口
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY); // 监听全部网卡
        addr.sin_port = htons(static_cast<uint16_t>(port));
        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            std::cerr << "bind failed: " << std::strerror(errno) << std::endl;
            return -1;
        }

        // 4.开启监听
        if (::listen(fd, LISTEN_BACKLOG) < 0) {
            std::cerr << "listen failed: " << std::strerror(errno) << std::endl;
            return -1;
        }
        std::cout << "Server listening on port " << port << " (fd=" << fd << ")" << std::endl;

        return guard.release();
    }

    // 客户端连接的线程入口
    // 循环读取消息，断开时清理
    void handle_client(int fd){
        try{ // 若抛出异常，捕获并打印
            // 1.寻找对应的 ClientInfo
            std::shared_ptr<ClientInfo> client;
            {
                std::lock_guard<std::mutex> lock(g_clients_mutex);
                auto it = g_clients_by_fd.find(fd);
                if (it == g_clients_by_fd.end()) {
                    std::cerr << "handle_client: fd " << fd << " not found" << std::endl;
                    ::close(fd);
                    return;
                }
                client = it->second;
            }

            // 2.循环读取消息
            while (true) {
                chatlab::Message msg;
                chatlab::ReadResult result = chatlab::read_message(fd, msg);
                // 成功收到消息
                if (result == chatlab::ReadResult::Ok) {
                    // 打印消息
                    std::cout << "[fd=" << fd << "]" 
                              << "type=" << static_cast<int>(msg.type) 
                              << " payload=" << msg.payload.dump() << std::endl;
                    continue;
                }
                // 失败
                if (result == chatlab::ReadResult::Closed) {
                    std::cout << "[fd=" << fd << "] closed by peer" << std::endl;
                    break;
                }
                if (result == chatlab::ReadResult::ProtocolError) {
                    std::cerr << "[fd=" << fd << "] protocol error" << std::endl;
                    break;
                }
                std::cerr << "[fd=" << fd << "] io error" << std::endl;
                break;
            }

            // 3.断开清理
            {
                std::lock_guard<std::mutex> lock(g_clients_mutex);
                // 若已登录，从用户名索引中删除
                if (client->logged_in) {
                    g_clients_by_name.erase(client->username);
                }
                // 从 fd 索引中删除
                g_clients_by_fd.erase(fd);
            }
            ::close(fd);
            std::cout << "[fd=" << fd << "] conection cleaned up" << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[fd=" << fd << "] exception: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[fd=" << fd << "] unknown exception" << std::endl;
        }
    }

} // namespace


int main(int argc, char* argv[]) {
    // 1.忽略 SIGPIPE
    chatlab::ignore_sigpipe();
    
    // 2.解析端口参数(默认9000)
    int port = DEFAULT_PORT;
    if (argc >= 2) {
        port = std::atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            std::cerr << "Invalid port: " << argv[1] << std::endl;
            return 1;
        }
    }
    
    // 3.创建 socket，开启监听
    int listen_fd = create_listen_socket(port);
    if (listen_fd < 0) {
        return 1;
    }
    chatlab::FdGuard listen_guard(listen_fd);

    // 4.接受连接
    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);

        int client_fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;  // 被信号打断
            }
            std::cerr << "accept() failed: " << std::strerror(errno) << std::endl;
            continue;  // 继续接受下一个连接
        }
        std::cout << "New connection: fd=" << client_fd << std::endl;

        // 创建 ClientInfo 插入 g_clients_by_fd
        auto client = std::make_shared<ClientInfo>();
        client->fd = client_fd;
        {
            std::lock_guard<std::mutex> lock(g_clients_mutex);
            g_clients_by_fd[client_fd] = client;
        }

        // 创建线程处理连接，独立运行
        std::thread(handle_client, client_fd).detach();
    }
    
    return 0;
}