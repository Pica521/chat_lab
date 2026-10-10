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
#include <vector>

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

    // 安全读取 JSON 字符串
    bool get_string_field(const nlohmann::json& payload, 
                          const std::string& key, 
                          std::string& out){
        // 检查 payload 是否为 JSON 对象
        if (!payload.is_object()) {
            return false;
        }     
        // 检查字段是否存在           
        if (!payload.contains(key)) {
            return false;
        }    
        // 检查字段是否为字符串
        if (!payload[key].is_string()) {
            return false;
        }
        out = payload[key].get<std::string>();
        return true;
    }

    // 给客户端带锁发送
    bool send_to_client(const std::shared_ptr<ClientInfo>& client, 
                        const chatlab::Message& msg){
        // 空指针检查
        if (!client) {
            return false;
        }
        // fd 有效性检查
        if (client->fd < 0) {
            return false;
        }
        // 加 send_mutex
        std::lock_guard<std::mutex> lock(client->send_mutex);
        // 调用 send_message
        return chatlab::send_message(client->fd, msg);
    }

    // 发送错误消息
    void send_error(const std::shared_ptr<ClientInfo>& client, 
                    int code, const std::string& message){
        chatlab::Message msg = chatlab::make_error(code, message);
        send_to_client(client, msg);
    }

    // 处理登录
    void handle_login(const std::shared_ptr<ClientInfo>& client, 
                      const chatlab::Message& msg){
        // 1.若已登录，拒绝重复登陆
        if (client->logged_in) {
            send_error(client, chatlab::ERR_USERNAME_INVALID, "already logged in");
            return;
        }

        // 2.读取 username
        std::string username;
        if (!get_string_field(msg.payload, "username", username)) {
            send_error(client, chatlab::ERR_FORMAT_ERROR, "missing or invalid username field");
            return;
        }

        // 3.检查用户名合法性
        constexpr std::size_t MAX_USERNAME_LENGTH = 32;
        if (username.empty()) {
            send_error(client, chatlab::ERR_USERNAME_INVALID, "username cannot be empty");
            return;
        }
        if (username.size() > MAX_USERNAME_LENGTH) {
            send_error(client, chatlab::ERR_USERNAME_INVALID, "username too long (max 32)");
            return;
        }

        // 4.加锁，查重，注册
        bool name_taken = false;
        {
            std::lock_guard<std::mutex> lock(g_clients_mutex);
            if (g_clients_by_name.count(username) > 0) {
                name_taken = true;
            } else {
                client->username = username;
                client->logged_in = true;
                g_clients_by_name[username] = client;
            }
        } // 锁外回复
        if (name_taken) {
            send_error(client, chatlab::ERR_USERNAME_EXISTS, "username already taken");
            return;
        }

        // 5.登录成功
        chatlab::Message resp;
        resp.type = chatlab::MSG_LOGIN_RESP;
        resp.payload["ok"] = true;
        resp.payload["reason"] = "welcome";
        send_to_client(client, resp);
        
        // 6.广播其他用户
        // 持锁收集在线客户端
        std::vector<std::shared_ptr<ClientInfo>> others;
        {
            std::lock_guard<std::mutex> lock(g_clients_mutex);
            for(auto& [name, info] : g_clients_by_name) {
                if (info->fd != client->fd) { // 排除自己
                    others.push_back(info);
                }
            }
        } // 锁外发送
        chatlab::Message notice;
        notice.type = chatlab::MSG_SYSTEM;
        notice.payload["content"] = username + " has joined";
        for (auto& info : others) {
            send_to_client(info, notice);
        }
    }

    // 处理在线列表请求
    void handle_list(const std::shared_ptr<ClientInfo>& client,
                     const chatlab::Message& msg) {
        (void)msg;
        // 1.拒绝未登录请求
        if (!client->logged_in) {
            send_error(client, chatlab::ERR_NOT_LOGGED_IN, "not logged in");
            return;
        }

        // 2.持锁收集已登录用户名
        std::vector<std::string> users;
        {
            std::lock_guard<std::mutex> lock(g_clients_mutex);
            for (const auto& [name, info] : g_clients_by_name) {
                users.push_back(name);
            }
        }

        // 3.回复消息
        chatlab::Message resp;
        resp.type = chatlab::MSG_LIST_RESP;
        resp.payload["users"] = users;
        send_to_client(client, resp);
    }

    // 处理群发
    void handle_broadcast(const std::shared_ptr<ClientInfo>& client,
                          const chatlab::Message& msg) {
        // 1.拒绝未登录请求
        if (!client->logged_in) {
            send_error(client, chatlab::ERR_NOT_LOGGED_IN, "not logged in");
            return;
        }
        
        // 2.读取 content
        std::string content;
        if (!get_string_field(msg.payload, "content", content)) {
            send_error(client, chatlab::ERR_FORMAT_ERROR, "missing or invalid content field");
            return;
        }
        
        // 3.检查 content
        if (content.size() > chatlab::MAX_CHAT_CONTENT_LENGTH) {
            send_error(client, chatlab::ERR_MESSAGE_TOO_LONG, "content too long");
            return;
        }
        
        // 4.持锁收集，遍历发送
        chatlab::Message bcast;
        bcast.type = chatlab::MSG_BROADCAST_RECV;
        bcast.payload["from"] = client->username;
        bcast.payload["content"] = content;
        std::vector<std::shared_ptr<ClientInfo>> targets;
        {
            std::lock_guard<std::mutex> lock(g_clients_mutex);
            for (auto [name,info] : g_clients_by_name) {
                targets.push_back(info);
            }
        }
        for (auto& target : targets) {
            send_to_client(target, bcast);
        }
    }

    // 处理私发
    void handle_private(const std::shared_ptr<ClientInfo>& client,
                        const chatlab::Message& msg) {
        // 1.拒绝未登录请求
        if (!client->logged_in) {
            send_error(client, chatlab::ERR_NOT_LOGGED_IN, "not logged in");
            return;
        }
        
        // 2.读取目标和内容
        std::string to;
        if (!get_string_field(msg.payload, "to", to)) {
            send_error(client, chatlab::ERR_FORMAT_ERROR, "missing or invalid to field");
            return;
        }
        std::string content;
        if (!get_string_field(msg.payload, "content", content)) {
            send_error(client, chatlab::ERR_FORMAT_ERROR, "missing or invalid content field");
            return;
        }
        
        // 3.检查 content
        if (content.size() > chatlab::MAX_CHAT_CONTENT_LENGTH) {
            send_error(client, chatlab::ERR_MESSAGE_TOO_LONG, "content too long");
            return;
        }
        
        // 4.持锁查找目标
        std::shared_ptr<ClientInfo> target;
        {
            std::lock_guard<std::mutex> lock(g_clients_mutex);
            auto it = g_clients_by_name.find(to);
            if (it != g_clients_by_name.end()) {
                target = it->second;
            }
        }  // 目标不存在
        if (!target) {
            send_error(client, chatlab::ERR_USER_NOT_ONLINE, "user not online: " + to);
            return;
        }
        
        // 5.发送消息
        chatlab::Message priv;
        priv.type = chatlab::MSG_PRIVATE_RECV;
        priv.payload["from"] = client->username;
        priv.payload["content"] = content;
        send_to_client(target, priv);
    }

    // 处理登出
    void handle_logout(const std::shared_ptr<ClientInfo>& client, 
                       const chatlab::Message& msg) {
        (void)msg;
        // 1.拒绝未登录请求
        if (!client->logged_in) {
            send_error(client, chatlab::ERR_NOT_LOGGED_IN, "not logged in");
            return;
        }

        // 2.加锁移除
        std::string leaving_name = client->username;  // 用于广播
        {
            std::lock_guard<std::mutex> lock(g_clients_mutex);
            g_clients_by_name.erase(client->username);
            client->username.clear();
            client->logged_in = false;
        }

        // 3.回复自己
        chatlab::Message bye;
        bye.type = chatlab::MSG_SYSTEM;
        bye.payload["content"] = "bye";
        send_to_client(client, bye);
        
        // 4.广播其他用户
        std::vector<std::shared_ptr<ClientInfo>> others;
        {
            std::lock_guard<std::mutex> lock(g_clients_mutex);
            for (auto& [name, info] : g_clients_by_name) {
                others.push_back(info);
            }
        }
        chatlab::Message notice;
        notice.type = chatlab::MSG_SYSTEM;
        notice.payload["content"] = leaving_name + " has left";
        for (auto& info : others) {
            send_to_client(info, notice);
        }
    }

    // 创建 socket，绑定端口，开启监听
    int create_listen_socket(int port) {
        // 1.创建 socket
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            std::cerr << "create socket failed: " << std::strerror(errno) << std::endl;
            return -1;
        }
        chatlab::FdGuard guard(fd);

        // 2.设置 SO_REUSEADDR
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
                    // 根据消息类型处理
                    switch (msg.type) {
                        case chatlab::MSG_LOGIN:
                            handle_login(client, msg);
                            break;
                        case chatlab::MSG_LIST:
                            handle_list(client, msg);
                            break;
                        case chatlab::MSG_BROADCAST:
                            handle_broadcast(client, msg);
                            break;
                        case chatlab::MSG_PRIVATE:
                            handle_private(client, msg);
                            break;
                        case chatlab::MSG_LOGOUT:
                            handle_logout(client, msg);
                            break;
                        default:
                            send_error(client, chatlab::ERR_UNKNOWN_TYPE, "unknown message type");
                            break;
                    }
                    continue;
                }
                // 失败
                if (result == chatlab::ReadResult::Closed) {  // 对端关闭
                    std::cout << "[fd=" << fd << "] closed by peer" << std::endl;
                    break;
                }
                if (result == chatlab::ReadResult::ProtocolError) {  // 协议错误
                    std::cerr << "[fd=" << fd << "] protocol error" << std::endl;
                    break;
                }
                std::cerr << "[fd=" << fd << "] io error" << std::endl;
                break;
            }

            // 3.断开清理
            std::string leaving_name;    // 用于系统广播通知
            bool was_logged_in = false;  // 判断 \quit 还是 docker kill
            {
                std::lock_guard<std::mutex> lock(g_clients_mutex);
                // 若已登录，从用户名索引中删除
                if (client->logged_in) {
                    leaving_name = client->username;
                    was_logged_in = true;
                    g_clients_by_name.erase(client->username);
                    client->logged_in = false;
                    client->username.clear();
                }
                // 从 fd 索引中删除
                g_clients_by_fd.erase(fd);
            }
            ::close(fd);
            // docker kill
            if (was_logged_in) {
                // 持锁收集在线用户
                std::vector<std::shared_ptr<ClientInfo>> others;
                {
                    std::lock_guard<std::mutex> lock(g_clients_mutex);
                    for (auto& [name, info] : g_clients_by_name) {
                        others.push_back(info);
                    }                
                }
                // 锁外发送
                chatlab::Message notice;
                notice.type = chatlab::MSG_SYSTEM;
                notice.payload["content"] = leaving_name + " has left";
                for (auto& info : others) {
                    send_to_client(info, notice);
                }
            }
            std::cout << "[fd=" << fd << "] connection cleaned up" << std::endl;
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