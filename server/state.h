#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>

// 本文件定义服务端的共享状态。
// 所有字段都由 AppState::mtx 保护；持锁时只做内存读写和拷贝。

// 一条消息
struct Message {
    uint64_t id = 0;     // 编号(全局)
    std::string from;    // 发送者用户名
    std::string to;      // 接收者用户名(群发为"*")
    std::string content; // 消息内容
    long long ts = 0;    // 发送时间
};

// 一个登录会话，一个 token 对应一个会话
struct Session {
    std::string username; // 会话所属用户名
    std::chrono::steady_clock::time_point last_seen; // 最后活跃时间(在线判定)
};

// 服务端全局状态，由所有连接线程共享
struct AppState {
    std::mutex mtx; // 保护容器
    // 用户表：用户名 -> "salt$sha256hex"
    std::map<std::string, std::string> users;
    // 会话表：token -> 会话
    std::map<std::string, Session> tokens;
    // 消息日志(保留最近的 MAX_LOG_SIZE 条)
    std::deque<Message> log;
    // 下一条消息编号
    uint64_t next_id = 1;
    // 有新消息时唤醒所有等待中的长轮询
    std::condition_variable cv;
};