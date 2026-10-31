#include <iostream>
#include <string>
#include <fstream>
#include <sstream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "common/net.h"

constexpr int PORT = 8080;

// 读取本地文件内容到字符串
std::string read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return "";  // 文件不存在
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

// (暂时)处理单个 HTTP 连接
void handle_http_client(int client_fd) {
    // 1. 接收浏览器发来的请求
    char buffer[4096] = {0};
    ssize_t bytes_read = ::recv(client_fd, buffer, sizeof(buffer) - 1, 0);
    if (bytes_read > 0) {
        std::string request(buffer, bytes_read);
        // 2. 检查请求行
        if (request.find("GET / ") == 0 || request.find("GET /index.html") == 0) {
            // 读入 HTML
            std::string html = read_file("web/index.html");
            // 3. 写响应
            if (!html.empty()) { //成功
                std::string response = "HTTP/1.1 200 OK\r\n";
                response += "Content-Type: text/html; charset=utf-8\r\n";
                response += "Content-Length: " + std::to_string(html.size()) + "\r\n";
                response += "Connection: close\r\n";
                response += "\r\n";
                response += html;
                // 发回浏览器
                chatlab::send_all(client_fd, response.data(), response.size());
            }
        } else { // 其他路径(暂时)404
                std::string not_found = "HTTP/1.1 404 Not Found\r\n"
                                        "Content-Length: 9\r\n"
                                        "Connection: close\r\n\r\n"
                                        "Not Found";
                chatlab::send_all(client_fd, not_found.data(), not_found.size());
            }
    }
    // 5. 关闭客户端连接
    ::close(client_fd);
}


int main(){
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
    if (::listen(listen_fd, 128) < 0) {
        std::cerr << "listen() failed" << std::endl;
        return 1;
    }
    std::cout << "HTTP Server listening on http://localhost:" << PORT << std::endl;

    // 5. 主循环，接收请求
    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_fd < 0) {
            continue;
        }

        // 处理 HTTP 请求
        handle_http_client(client_fd);
    }

    ::close(listen_fd);
    return 0;
}