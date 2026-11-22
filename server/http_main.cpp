#include <cerrno>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common/net.h"
#include "server/http.h"
#include "server/state.h"

constexpr int PORT = 8080;
constexpr int LISTEN_BACKLOG = 128;
// 所有线程连接共享的全局状态
static AppState g_state;

int main(){
    chatlab::ignore_sigpipe();
    // 1. 创建监听 socket
    int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::cerr << "socket() failed" << std::endl;
        return 1;
    }

    // 2. 设置端口复用
    int opt = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 3. 绑定地址和端口
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(PORT);

    if(::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "bind() failed on port " << PORT << std::endl;
        return 1;
    }

    // 4. 开始监听
    if (::listen(listen_fd, LISTEN_BACKLOG) < 0) {
        std::cerr << "listen() failed" << std::endl;
        return 1;
    }
    std::cout << "HTTP Server listening on http://localhost:" << PORT << std::endl;

    // 5. 主循环：接收请求，每连接创建一个线程
    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "accept() failed: " << std::strerror(errno) << std::endl;
            continue;
        }

        // 每连接一个线程
        try{
            std::thread(handle_http_client, client_fd, std::ref(g_state)).detach();
        } catch (const std::exception& e) {
            std::cerr << "create thread failed: " << e.what() << std::endl;
            ::close(client_fd); // 线程没起来，fd 由主线程负责关闭 
        }
    }

    ::close(listen_fd);
    return 0;
}