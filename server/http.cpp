#include "server/http.h"
#include "server/api.h"
#include "common/net.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <sys/socket.h>
#include <unistd.h>

// 读取本地文件内容
static std::string read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return "";
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

// 拼装并发送 HTTP 响应
void send_http_response(int fd, int status_code, const std::string& status_text,
                                const std::string& content_type, const std::string& body) {
    std::string response = "HTTP/1.1 " + std::to_string(status_code) + " " + status_text + "\r\n";
    response += "Content-Type: " + content_type + "\r\n";
    response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    response += "Connection: close\r\n\r\n";
    response += body;
    chatlab::send_all(fd, response.data(), response.size());
}

// 解析 HTTP 请求
static bool parse_http_request(int fd, HttpRequest& req) {
    std::string raw_data;
    char buffer[4096];

    // 1. 循环读，直到 Header 结束标志 "\r\n\r\n"
    size_t header_end_pos = std::string::npos;
    while (header_end_pos == std::string::npos) {
        ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0) return false;  // 断开或出错
        raw_data.append(buffer, n);
        header_end_pos = raw_data.find("\r\n\r\n"); 
    }

    // 2. 切分 Header 和 Body
    std::string header_part = raw_data.substr(0, header_end_pos);
    req.body = raw_data.substr(header_end_pos + 4);  // 跳过 "\r\n\r\n"

    // 3. 解析请求行
    std::istringstream header_stream(header_part);
    std::string request_line;
    std::getline(header_stream, request_line);

    std::istringstream line_stream(request_line);
    std::string http_version;
    line_stream >> req.method >> req.path >> http_version;

    //4. 解析剩余 Header
    std::string line;
    size_t content_length = 0;
    while (std::getline(header_stream, line)) {
        if (line.back() == '\r') line.pop_back();  // 去除 \r
        size_t colon_pos = line.find(':');
        if (colon_pos != std::string::npos) {
            std::string key = line.substr(0, colon_pos);
            std::string value = line.substr(colon_pos + 1);
            // 去除前导空格
            size_t first = value.find_first_not_of(' ');  
            if (first != std::string::npos) value = value.substr(first);

            req.headers[key] = value;
            if (key == "Content-Length") {
                content_length = std::stoul(value);
            }
        }
    }

    // 5. 若 Body 没收满就继续读
    if (req.body.size() < content_length) {
        size_t remaining = content_length - req.body.size();
        std::string extra_body(remaining, '\0');
        if (chatlab::recv_exact(fd, &extra_body[0], remaining)) {
            req.body += extra_body;
        } else {
            return false;
        }
    }
    return true;
}

// 路由分发
void handle_http_client(int client_fd) {
    HttpRequest req;
    if (!parse_http_request(client_fd, req)) {
        ::close(client_fd);
        return;
    }

    // 1. 处理静态文件请求 (GET)
    if (req.method == "GET") {
        if (req.path == "/" || req.path == "/index.html") {
            send_http_response(client_fd, 200, "OK", "text/html; charset=utf-8", read_file("web/index.html"));
        } else if (req.path == "/style.css") {
            send_http_response(client_fd, 200, "OK", "text/css", read_file("web/style.css"));
        } else if (req.path == "/app.js") {
            send_http_response(client_fd, 200, "OK", "application/javascript", read_file("web/app.js"));
        } else {
            send_http_response(client_fd, 404, "Not Found", "text/plain", "404 Not Found");
        }
    }
    
    // 2. 处理 API 请求 (POST)
    else if (req.method == "POST") {
        if (req.path == "/api/register") {
            std::string resp = handle_api_register(req.body);
            send_http_response(client_fd, 200, "OK", "application/json", resp);
        } else if (req.path == "/api/login") {
            std::string resp = handle_api_login(req.body);
            send_http_response(client_fd, 200, "OK", "application/json", resp);
        } else {
            send_http_response(client_fd, 404, "Not Found", "application/json", "{\"code\":404, \"msg\":\"API Not Found\"}");
        }
    } else {
        send_http_response(client_fd, 405, "Method Not Allowed", "text/plain", "405 Method Not Allowed");
    }

    ::close(client_fd);
}