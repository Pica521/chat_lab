#include "server/http.h"
#include "server/api.h"
#include "common/net.h"

#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <nlohmann/json.hpp>
// 该文件处理网络和 HTTP 协议
// 读取并解析请求、按路径分发、拼装并发送响应

// 访问日志的互斥锁
static std::mutex g_log_mutex;

// 请求解析结果
enum class ParseResult {
    Ok,             // 解析成功
    Closed,         // 对端关闭或超时
    BadRequest,     // 请求格式错误
    HeaderTooLarge, // 请求头超上限
    BodyTooLarge,   //请求体超上限
};

// 状态码
static const char* status_text(int code) {
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 422: return "Unprocessable Entity";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        default:  return "Unknown";
    }
}

// 打印一行访问日志 (status 为 0 表示没有发送响应)
static void log_request(const std::string& method, const std::string& path, int status, long long elapsed_ms) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::cout << "[HTTP] "
              << (method.empty() ? "-" : method) << " "
              << (path.empty() ? "-" : path) << " -> ";
    if (status == 0) {
        std::cout << "(no response)";
    } else {
        std::cout<< status;
    }
    std::cout << " " << elapsed_ms << " ms" << std::endl;
}

// 去掉字符串首尾的空格和制表符
static std::string trim(const std::string& s) {
    const char* whitespace = " \t";
    std::size_t begin = s.find_first_not_of(whitespace);
    if (begin == std::string::npos) {
        return "";
    }
    std::size_t end = s.find_last_not_of(whitespace);
    return s.substr(begin, end - begin + 1);
}

// 转成小写（HTTP 头字段名大小写不敏感）
static std::string to_lower(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// 把全数字字符串解析成无符号整数，失败返回 false
static bool parse_size(const std::string& text, std::size_t& out) {
    if (text.empty() || text.size() > 20) {
        return false;
    }
    for (char c : text) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    try {
        unsigned long long value = std::stoull(text);
        if (value > 0xFFFFFFFFULL) {  // 超过 4 GiB 视为非法，防止天文数字
            return false;
        }
        out = static_cast<std::size_t>(value);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// 解析查询串，如 "since=5&x=1"
// 不做百分号解码，本项目的查询参数只有数字
static void parse_query(const std::string& query, std::map<std::string, std::string>& out) {
    std::size_t pos = 0;
    while (pos <= query.size()) {
        std::size_t amp = query.find('&', pos);
        std::string pair = query.substr(pos, (amp == std::string::npos ? query.size() : amp) - pos);
        if (!pair.empty()) {
            std::size_t eq = pair.find('=');
            if (eq == std::string::npos) {
                out[pair] = "";
            } else {
                out[pair.substr(0, eq)] = pair.substr(eq + 1);
            }
        }
        if (amp == std::string::npos) {
            break;
        }
        pos = amp + 1;
    }
}

// 解析请求行：METHOD PATH HTTP/1.x，并拆出查询串
static bool parse_request_line(const std::string& line, HttpRequest& req) {
    std::istringstream stream(line);
    std::string version;
    if (!(stream >> req.method >> req.path >> version)) {
        return false;
    }
    std::string extra;
    if (stream >> extra) {  // 请求行只允许三个字段
        return false;
    }
    if (version != "HTTP/1.1" && version != "HTTP/1.0") {
        return false;  // 只支持 HTTP/1.0 和 HTTP/1.1
    }
    if (req.method.empty() || req.path.empty() || req.path[0] != '/') {
        return false;  // 不支持绝对 URI 形式
    }

    // 拆分路径与查询串
    std::size_t question = req.path.find('?');
    if (question != std::string::npos) {
        std::string query = req.path.substr(question + 1);
        req.path = req.path.substr(0, question);
        parse_query(query, req.query);
    }
    return true;
}

// 解析请求头字段，同时取出 Content-Length
static bool parse_headers(const std::string& header_part, std::size_t first_line_end,
                          HttpRequest& req, std::size_t& content_length,
                          bool& has_content_length) {
    std::size_t pos = (first_line_end == std::string::npos)
                          ? header_part.size()
                          : first_line_end + 2;  // 跳过请求行和它后面的 "\r\n"
    while (pos < header_part.size()) {
        std::size_t end = header_part.find("\r\n", pos);
        std::string line =
            header_part.substr(pos, (end == std::string::npos ? header_part.size() : end) - pos);
        pos = (end == std::string::npos) ? header_part.size() : end + 2;

        if (line.empty()) {
            continue;
        }
        std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            return false;  // 头部字段必须有冒号
        }
        std::string key = to_lower(trim(line.substr(0, colon)));
        std::string value = trim(line.substr(colon + 1));
        if (key.empty()) {
            return false;
        }
        if (key == "transfer-encoding") {
            return false;  // 不支持 chunked，只接受 Content-Length
        }
        if (key == "content-length") {
            if (has_content_length) {
                return false;  // 重复的 Content-Length 视为非法
            }
            if (!parse_size(value, content_length)) {
                return false;
            }
            has_content_length = true;
        }
        req.headers[key] = value;
    }
    return true;
}

// 读取请求头，直到出现 "\r\n\r\n"
// 返回 false 表示读失败；too_large 表示因为超过上限而失败
static bool read_http_header(int fd, std::string& raw, bool& too_large) {
    char buffer[4096];
    too_large = false;
    while (true) {
        std::size_t pos = raw.find("\r\n\r\n");
        if (pos != std::string::npos) {
            if (pos + 4 > MAX_HEADER_SIZE) {
                too_large = true;
                return false;
            }
            return true;
        }
        if (raw.size() >= MAX_HEADER_SIZE) {
            too_large = true;
            return false;
        }

        ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;  // 被信号打断，重试
            }
            return false;  // 读超时或网络错误
        }
        if (n == 0) {
            return false;  // 对端关闭
        }
        raw.append(buffer, static_cast<std::size_t>(n));
    }
}

// 读取本地文件内容，失败返回空串
static std::string read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return "";
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

// 拼装错误响应体：{code, msg}
static std::string make_error_json(int code, const std::string& msg) {
    nlohmann::json body;
    body["code"] = code;
    body["msg"] = msg;
    return body.dump();
}

// 发送 JSON 错误响应
static void send_json_error(int fd, int status, const std::string& msg) {
    send_http_response(fd, status, "application/json; charset=utf-8", make_error_json(status, msg), true);
}

// 有限排空输入缓冲区：最多丢弃 1 MiB 或 200 毫秒
// 目的：请求被拒绝时，避免因为还有未读数据而在 close 时触发 RST，
//       导致对端收不到刚才发送的错误响应
static void drain_input(int fd) {
    timeval tiny{};
    tiny.tv_sec = 0;
    tiny.tv_usec = 200 * 1000;  // 200 ms
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tiny, sizeof(tiny));

    char buffer[4096];
    std::size_t total = 0;
    while (total < 1024 * 1024) {
        ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0) {
            break;
        }
        total += static_cast<std::size_t>(n);
    }
}

// 拼装并发送 HTTP 响应
void send_http_response(int fd, int status_code, const std::string& content_type,
                        const std::string& body, bool no_store) {
    std::string response = "HTTP/1.1 " + std::to_string(status_code) + " " +
                           status_text(status_code) + "\r\n";
    response += "Content-Type: " + content_type + "\r\n";
    response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    response += "Connection: close\r\n";
    if (no_store) {
        response += "Cache-Control: no-store\r\n";
    }
    response += "\r\n";
    response += body;
    chatlab::send_all(fd, response.data(), response.size());
}

// 解析一条完整的 HTTP 请求（头部 + 按 Content-Length 读取 Body）
static ParseResult parse_http_request(int fd, HttpRequest& req) {
    // 1. 读请求头
    std::string raw;
    bool header_too_large = false;
    if (!read_http_header(fd, raw, header_too_large)) {
        return header_too_large ? ParseResult::HeaderTooLarge : ParseResult::Closed;
    }

    // 2. 切分头部与已经读到的 body 片段
    const std::size_t header_end = raw.find("\r\n\r\n");
    std::string header_part = raw.substr(0, header_end);
    std::string body = raw.substr(header_end + 4);

    // 3. 请求行
    std::size_t line_end = header_part.find("\r\n");
    std::string request_line =
        (line_end == std::string::npos) ? header_part : header_part.substr(0, line_end);
    if (!parse_request_line(request_line, req)) {
        return ParseResult::BadRequest;
    }

    // 4. 头部字段
    std::size_t content_length = 0;
    bool has_content_length = false;
    if (!parse_headers(header_part, line_end, req, content_length, has_content_length)) {
        return ParseResult::BadRequest;
    }

    // 5. Body
    if (req.method == "POST" && !has_content_length) {
        return ParseResult::BadRequest;  // POST 必须带 Content-Length
    }
    if (has_content_length) {
        if (content_length > MAX_BODY_SIZE) {
            return ParseResult::BodyTooLarge;  // 先拒绝，不再读取 body
        }
        if (body.size() > content_length) {
            return ParseResult::BadRequest;  // 不支持管道化，多出来的数据无法归属
        }
        if (body.size() < content_length) {
            // 客户端可能在等 100-continue：先回一个，否则它会等约 1 秒才发 body
            auto expect = req.headers.find("expect");
            if (expect != req.headers.end() && to_lower(expect->second) == "100-continue") {
                static const char kContinue[] = "HTTP/1.1 100 Continue\r\n\r\n";
                chatlab::send_all(fd, kContinue, sizeof(kContinue) - 1);
            }
            const std::size_t remaining = content_length - body.size();
            std::string extra(remaining, '\0');
            if (!chatlab::recv_exact(fd, extra.data(), remaining)) {
                return ParseResult::Closed;
            }
            body += extra;
        }
        req.body = std::move(body);
    }
    return ParseResult::Ok;
}

// 静态资源白名单：只允许这三个文件，避免路径穿越
struct StaticFile {
    const char* path;
    const char* file;
    const char* content_type;
};
static const StaticFile kStaticFiles[] = {
    {"/", "web/index.html", "text/html; charset=utf-8"},
    {"/index.html", "web/index.html", "text/html; charset=utf-8"},
    {"/style.css", "web/style.css", "text/css; charset=utf-8"},
    {"/app.js", "web/app.js", "application/javascript; charset=utf-8"},
};

// API 路由表
using ApiHandler = std::string (*)(const std::string&, AppState&);
struct ApiRoute {
    const char* method;
    const char* path;
    ApiHandler handler;
};
static const ApiRoute kApiRoutes[] = {
    {"POST", "/api/register", handle_api_register},
    {"POST", "/api/login", handle_api_login},
};

// 路由分发，返回实际发送的状态码（用于日志）
static int dispatch_request(int fd, const HttpRequest& req, AppState& state) {
    // 1. 静态资源
    for (const auto& file : kStaticFiles) {
        if (req.path == file.path) {
            if (req.method != "GET") {
                send_json_error(fd, 405, "方法不允许");
                return 405;
            }
            std::string content = read_file(file.file);
            if (content.empty()) {
                send_json_error(fd, 404, "文件不存在");
                return 404;
            }
            send_http_response(fd, 200, file.content_type, content);
            return 200;
        }
    }

    // 2. API
    for (const auto& route : kApiRoutes) {
        if (req.path == route.path) {
            if (req.method != route.method) {
                send_json_error(fd, 405, "方法不允许");
                return 405;
            }
            std::string resp = route.handler(req.body, state);
            send_http_response(fd, 200, "application/json; charset=utf-8", resp, true);
            return 200;
        }
    }

    // 3. 都不匹配
    send_json_error(fd, 404, "接口不存在");
    return 404;
}

// 处理单个 HTTP 连接：读请求 -> 解析 -> 分发 -> 关闭
void handle_http_client(int client_fd, AppState& state) {
    const auto begin = std::chrono::steady_clock::now();

    // 设置接收超时：只连不发、或发一半就停的客户端不能长期占住线程
    timeval tv{};
    tv.tv_sec = RECV_TIMEOUT_SECONDS;
    tv.tv_usec = 0;
    ::setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    HttpRequest req;
    int status = 0;           // 实际发送的状态码，0 表示没有响应
    bool need_drain = false;  // 被拒绝且请求体还没读完时为 true

    try {
        ParseResult result = parse_http_request(client_fd, req);
        switch (result) {
            case ParseResult::Ok:
                status = dispatch_request(client_fd, req, state);
                break;
            case ParseResult::Closed:
                // 对端关闭或读超时：不回复，直接关连接
                break;
            case ParseResult::HeaderTooLarge:
                status = 431;
                send_json_error(client_fd, 431, "请求头过大");
                need_drain = true;
                break;
            case ParseResult::BodyTooLarge:
                status = 413;
                send_json_error(client_fd, 413, "请求体过大");
                need_drain = true;
                break;
            case ParseResult::BadRequest:
                status = 400;
                send_json_error(client_fd, 400, "请求格式错误");
                need_drain = true;
                break;
        }
    } catch (const std::exception& e) {
        // 任何异常都只影响当前连接，不能让整个进程退出
        std::cerr << "[HTTP] exception: " << e.what() << std::endl;
        if (status == 0) {
            status = 500;
            send_json_error(client_fd, 500, "服务器内部错误");
        }
    }

    // 拒绝请求时先半关闭再有限排空，避免 RST 把刚才的错误响应冲掉
    if (need_drain) {
        ::shutdown(client_fd, SHUT_WR);
        drain_input(client_fd);
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - begin)
                             .count();
    log_request(req.method, req.path, status, static_cast<long long>(elapsed));

    ::close(client_fd);
}