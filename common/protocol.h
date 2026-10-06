#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace chatlab{
    // 本文件定义了聊天协议的帧格式、消息类型、错误码,以及消息的编解码接口。

    // 帧头常量
    constexpr uint16_t MAGIC = 0x434C;
    constexpr uint8_t  PROTOCOL_VERSION = 0x01;
    constexpr std::size_t HEADER_SIZE = 8;
    constexpr uint32_t MAX_PROTOCOL_LENGTH = 4 * 1024 *1024; // 4 MiB
    constexpr std::size_t MAX_CHAT_CONTENT_LENGTH = 1 * 1024 * 1024; // 1 MiB
    // 文件块上限，阶段三暂留

    // 消息类型
    // 客户端 -> 服务端
    constexpr uint8_t MSG_LOGIN     = 0x01;  // 登录请求
    constexpr uint8_t MSG_LOGOUT    = 0x02;  // 主动退出
    constexpr uint8_t MSG_LIST      = 0x03;  // 请求在线列表
    constexpr uint8_t MSG_BROADCAST = 0x04;  // 群发
    constexpr uint8_t MSG_PRIVATE   = 0x05;  // 私聊
    constexpr uint8_t MSG_FILE_SEND = 0x10;  // 发文件
    // 服务端 -> 客户端
    constexpr uint8_t MSG_LOGIN_RESP     = 0x81;  // 登录响应
    constexpr uint8_t MSG_LIST_RESP      = 0x82;  // 在线列表
    constexpr uint8_t MSG_BROADCAST_RECV = 0x83;  // 收到群发
    constexpr uint8_t MSG_PRIVATE_RECV   = 0x84;  // 收到私聊
    constexpr uint8_t MSG_SYSTEM         = 0x85;  // 系统通知
    constexpr uint8_t MSG_ERROR          = 0x86;  // 错误
    constexpr uint8_t MSG_FILE_RECV      = 0x90;  // 收文件

    // 错误码
    constexpr int ERR_USERNAME_EXISTS  = 1001;  // 用户名已存在
    constexpr int ERR_USERNAME_INVALID = 1002;  // 用户名非法
    constexpr int ERR_NOT_LOGGED_IN    = 1003;  // 未登录就发送业务消息
    constexpr int ERR_USER_NOT_ONLINE  = 1004;  // 私聊目标不在线
    constexpr int ERR_MESSAGE_TOO_LONG = 1005;  // 消息内容超过业务层上限
    constexpr int ERR_FORMAT_ERROR     = 1006;  // JSON 解析失败、字段缺失
    constexpr int ERR_UNKNOWN_TYPE     = 1007;  // 消息类型不认识
    constexpr int ERR_LENGTH_TOO_LARGE = 1008;  // 协议层长度超限(关闭连接)
    constexpr int ERR_BAD_MAGIC        = 1009;  // 魔数错误(关闭连接)
    constexpr int ERR_BAD_VERSION      = 1010;  // 版本不支持(关闭连接)

    // 消息结构体，表示一条消息
    struct Message {
        uint8_t type = 0; // 消息类型
        nlohmann::json payload = nlohmann::json::object();
    };

    // 编码:把 Message 编码为字节流
    std::string encode(const Message& msg);
    // 解码结果
    enum class DecodeResult {
        Ok,              // 解析成功
        BadMagic,        // 魔数不匹配
        BadVersion,      // 版本号不支持
        LengthTooLarge,  // LENGTH 超过协议层上限
        BadJson,         // 不合法 JSON
    };
    // 解码
    DecodeResult decode(const std::string& header, 
                        const std::string& payload, 
                        Message& out);
    
    // 读写结果
    enum class ReadResult {
        Ok,             // 成功读到一条完整消息
        Closed,         // 对端关闭连接
        ProtocolError,  // 协议错误
        IoError,        // 网络读写错误
    };
    // 读写
    ReadResult read_message(int fd, Message& out);
    bool send_message(int fd, const Message& msg); // true为成功

    // 错误信息
    Message make_error(int code, const std::string& message);
} // namespace chatlab