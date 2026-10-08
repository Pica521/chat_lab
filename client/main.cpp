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
#include <iomanip>
#include <mutex>
#include <string>
#include <thread>
#include <atomic>
#include <cstdlib>
#include <chrono>

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

    // 连接服务器
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

    // 去掉字符串首尾的空白字符
    std::string trim(const std::string& s){
        const std::string whitespace = " \n\t\r";
        std::size_t start = s.find_first_not_of(whitespace);  // 第一个非空字符的位置
        if (start == std::string::npos) {
            // 全是空白
            return "";
        }
        std::size_t end = s.find_last_not_of(whitespace);  // 最后一个非空字符的位置
        return s.substr(start, end - start + 1);
    }

    // 提示输入用户名
    std::string prompt_username() {
        std::string line;
        while (true) {
            std::cout << "Enter username: ";
            std::cout.flush();
            // EOF 或读取错误
            if (!std::getline(std::cin, line)) {
                std::cerr << "\nInput closed, exiting." << std::endl;
                std::exit(1);
            }
            // 去掉首尾空白字符
            std::string trimmed = trim(line);
            if (trimmed.empty()) {
                continue;
            }
            return trimmed;
        }
    }

    // 发送登录消息
    bool send_login(int fd, const std::string& username) {
        chatlab::Message msg;
        msg.type = chatlab::MSG_LOGIN;
        msg.payload["username"] = username;
        return chatlab::send_message(fd, msg);
    }

    // 发送用户列表请求
    bool send_list(int fd) {
        chatlab::Message msg;
        msg.type = chatlab::MSG_LIST;
        msg.payload = nlohmann::json::object();
        return chatlab::send_message(fd, msg);
    }

    // 发送群发消息
    bool send_broadcast(int fd, const std::string& content) {
        chatlab::Message msg;
        msg.type = chatlab::MSG_BROADCAST;
        msg.payload["content"] = content;
        return chatlab::send_message(fd, msg);
    }

    // 发送私聊消息
    bool send_private(int fd, const std::string& to, const std::string& content) {
        chatlab::Message msg;
        msg.type = chatlab::MSG_PRIVATE;
        msg.payload["to"] = to;
        msg.payload["content"] = content;
        return chatlab::send_message(fd, msg);
    }

    // 发送退出登录
    bool send_logout(int fd) {
        chatlab::Message msg;
        msg.type = chatlab::MSG_LOGOUT;
        msg.payload = nlohmann::json::object();
        return chatlab::send_message(fd, msg);
    }

    // 等待服务器回复 LOGIN_RESP
    bool wait_login_response(int fd) {
        while (true) {
            chatlab::Message msg;
            chatlab::ReadResult result = chatlab::read_message(fd, msg);
            // 读取失败
            if (result == chatlab::ReadResult::Closed) {
                std::cerr << "Connection closed before login response" << std::endl;
                return false;
            }
            if (result == chatlab::ReadResult::ProtocolError) {
                std::cerr << "Protocol error while waiting for login response" << std::endl;
                return false;
            }
            if (result == chatlab::ReadResult::IoError) {
                std::cerr << "IO error while waiting for login response" << std::endl;
                return false;
            }
            // 收到回复
            if (msg.type == chatlab::MSG_LOGIN_RESP) {
                bool ok = msg.payload.value("ok", false);
                std::string reason = msg.payload.value("reason", "");
                if (ok) {
                    std::cout << "Login successful: " << reason << std::endl;
                    return true;
                } else {
                    std::cerr << "Login failed: " << reason << std::endl;
                    return false;
                }
            }
            // 收到 ERROR
            if (msg.type == chatlab::MSG_ERROR) {
                int code = msg.payload.value("code", 0);
                std::string message = msg.payload.value("message", "");
                std::cerr << "Server error " << code << ": " << message << std::endl;
                return false;
            }
            // 若先收到了其他消息
            std::cout << "[ignored] type=0x" << std::hex << static_cast<int>(msg.type) 
                      << std::dec << " payload=" << msg.payload.dump() << std::endl;
        }
    }

    // 打印接受到的消息
    void print_message(const chatlab::Message& msg) {
        // 加锁
        std::lock_guard<std::mutex> lock(g_cout_mutex);
        // 打印消息
        switch (msg.type) {
            case chatlab::MSG_BROADCAST_RECV: {
                std::string from = msg.payload.value("from", "?");
                std::string content = msg.payload.value("content", "");
                std::cout << "[broadcast] " << from << ": " << content << std::endl;
                break;
            }
            case chatlab::MSG_PRIVATE_RECV: {
                std::string from = msg.payload.value("from", "?");
                std::string content = msg.payload.value("content", "");
                std::cout << "[private] " << from << ": " << content << std::endl;
                break;
            }
            case chatlab::MSG_SYSTEM: {
                std::string content = msg.payload.value("content", "");
                std::cout << "[system] " << content << std::endl;
                break;
            }
            case chatlab::MSG_ERROR: {
                int code = msg.payload.value("code", 0);
                std::string message = msg.payload.value("message", "");
                std::cerr << "[error] " << code << ": " << message << std::endl;
                break;
            }
            case chatlab::MSG_LIST_RESP: {
                std::cout<< "Online users:";
                if (msg.payload.contains("users") && msg.payload["users"].is_array()) {
                    for (const auto& user : msg.payload["users"]) {
                        std::cout << " " << user.get<std::string>();
                    }
                }
                std::cout << std::endl;
                break;
            }
            default:
                std::cout << "[unknown] type=0x" << std::hex << static_cast<int>(msg.type) 
                          << std::dec << " payload=" << msg.payload.dump() << std::endl;
                break;
        }
    }

    // 接收线程
    void receiver_thread(int fd) {
        {   // 加锁，线程启动提示
            std::lock_guard<std::mutex> lock(g_cout_mutex);
            std::cout << "[receiver] thread started" << std::endl;
        }
        // 循环读取
        while (g_running) {
            chatlab::Message msg;
            chatlab::ReadResult result = chatlab::read_message(fd, msg);
            // 成功读取
            if (result == chatlab::ReadResult::Ok) {
                print_message(msg);
                continue;
            }
            // 读取失败
            {
                std::lock_guard<std::mutex> lock(g_cout_mutex);
                if (result == chatlab::ReadResult::Closed) {
                    std::cout<< "[receiver] connection closed by server" << std::endl;
                } else if (result == chatlab::ReadResult::ProtocolError) {
                    std::cerr << "[receiver] protocol error" << std::endl;
                } else {
                    std::cerr << "[receiver] io error" << std::endl;
                }
                std::cout << "[receiver] thread exiting" << std::endl;
            }
            g_running = false;
            std::exit(0);
        }
        // 线程退出提示
        {
            std::lock_guard<std::mutex> lock(g_cout_mutex);
            std::cout << "[receiver] thread exiting" << std::endl;
        }
    }

    // 处理用户输入
    bool handle_command(int fd, const std::string& line) {
        // 加锁
        auto print_line = [](const std::string& s) {
            std::lock_guard<std::mutex> lock(g_cout_mutex);
            std::cout << s << std::endl;
        };
        
        // 类型 /list
        if (line == "/list") {
            return send_list(fd);
        }
        // 类型 /broadcast <message>
        if (line.rfind("/broadcast ", 0) == 0) {
            std::string content = line.substr(11);  // 去除 "/broadcast "
            if (content.empty()) {
                print_line("Usage: /broadcast <message>");
                return true;
            }
            return send_broadcast(fd, content);
        }
        // 类型 /msg <user> <message>
        if (line.rfind("/msg ", 0) == 0) {
            std::string rest = line.substr(5);  // 去掉 "/msg "
            std::size_t space = rest.find(' ');
            if (space == std::string::npos) {
                print_line("Usage: /msg <user> <message>");
                return true;
            }
            std::string to = rest.substr(0, space);
            std::string content = rest.substr(space + 1);
            if (to.empty() || content.empty()) {
                print_line("Usage: /msg <user> <message>");
                return true;
            }
            return send_private(fd, to, content);
        }
        // 类型 /quit
        if (line == "/quit") {
            send_logout(fd);
            print_line("Bye.");
            return false;
        }
        // 未知类型
        print_line("Unknown command. Try /list, /broadcast, /msg.");
        return true;
    }

    // 循环读取用户输入
    void user_input_loop(int fd) {
        std::string line;
        while (g_running) {
            // 加锁，打印提示符
            {
                std::lock_guard<std::mutex> lock(g_cout_mutex);
                std::cout << "> ";
                std::cout.flush();
            }
            // EOF 或读取错误
            if (!std::getline(std::cin, line)) {
                std::lock_guard<std::mutex> lock(g_cout_mutex);
                std::cout << "\nInput closed, exiting." << std::endl;
                break;
            }
            // 清理首位空字符
            std::string trimmed = trim(line);
            if (trimmed.empty()) {
                continue;
            }
            // 处理输入
            if (!handle_command(fd, trimmed)) {
                // 失败
                break;
            }
        }
        // 退出循环提示
        {
            std::lock_guard<std::mutex> lock(g_cout_mutex);
            std::cout << "Input loop ended." << std::endl;
        }
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

    // 提示输入用户名
    std::string username = prompt_username();

    // 发送 LOGIN 并等待 LOGIN_RESP
    if (!send_login(fd, username)) {
        std::cerr << "Failed to send login" << std::endl;
        ::close(fd);
        return 1;
    }
    if (!wait_login_response(fd)) {
        std::cerr << "Login failed" << std::endl;
        ::close(fd);
        return 1;
    }

    // 启动接收线程
    std::thread receiver(receiver_thread, fd);

    // 进入用户输入循环
    user_input_loop(fd);

    // 通知并等待接收线程退出
    g_running = false;
    ::shutdown(fd, SHUT_RDWR);
    receiver.join();

    // 关闭连接
    ::close(fd);
    std::cout << "Client exiting." <<std::endl;
    return 0;
}