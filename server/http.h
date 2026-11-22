#pragma once
#include <string>
#include <map>
#include <cstddef>

struct AppState;

// 传输层上限
constexpr std::size_t MAX_HEADER_SIZE = 16 * 1024;  // 请求头总长上限：16 KiB
constexpr std::size_t MAX_BODY_SIZE = 64 * 1024;    // 请求体上限：64 KiB
constexpr int RECV_TIMEOUT_SECONDS = 15;            // 单次 recv 超时（秒）

// 定义 HTTP 请求结构体
struct HttpRequest {
    std::string method;  // 请求方式(GET, POST...)
    std::string path;  // 请求路径
    std::map<std::string, std::string> query; // 查询参数
    std::map<std::string, std::string> headers;  // 请求头
    std::string body;  //请求体
};

// 处理单个 HTTP 连接
void handle_http_client(int client_fd, AppState& state);

// 发送 HTTP 响应
// no_store 为 true 时附加 Cache-Control: no-store
void send_http_response(int fd, int status_code, const std::string& content_type, const std::string& body, bool no_store = false);