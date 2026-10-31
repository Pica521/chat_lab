#pragma once
#include <string>
#include <map>

// 定义 HTTP 请求结构体
struct HttpRequest {
    std::string method;  // 请求方式(GET, POST...)
    std::string path;  // 请求路径
    std::map<std::string, std::string> headers;  // 请求头
    std::string body;  //请求体
};

// 处理单个 HTTP 连接
void handle_http_client(int client_fd);

// 发送响应的辅助函数
void send_http_response(int fd, int status_code,const std::string& status_text, const std::string& content_type, const std::string& body);