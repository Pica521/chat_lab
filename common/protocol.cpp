#include "common/protocol.h"
#include "common/net.h"

#include <cstdint>
#include <string>

// 实现 protocol.h 中声明的编解码与读写
namespace chatlab{
    // encode
    std::string encode(const Message& msg){
        // 1.把 JSON 负载序列化成字符串
        std::string payload = msg.payload.dump();
        // 2.检查负载长度
        if (payload.size() > MAX_PROTOCOL_LENGTH) {
            return {};
        }
        // 3.头长 + 负载
        std::string frame;
        frame.reserve(HEADER_SIZE + payload.size());
        // 4.大端序拼接头部
        frame.push_back(static_cast<char>((MAGIC >> 8) & 0xFF));
        frame.push_back(static_cast<char>(MAGIC & 0xFF));
        frame.push_back(static_cast<char>(PROTOCOL_VERSION));
        frame.push_back(static_cast<char>(msg.type));
        uint32_t len = static_cast<uint32_t>(payload.size());
        frame.push_back(static_cast<char>((len >> 24) & 0xFF));
        frame.push_back(static_cast<char>((len >> 16) & 0xFF));
        frame.push_back(static_cast<char>((len >> 8) & 0xFF));
        frame.push_back(static_cast<char>(len & 0xFF));
        // 5.拼接
        frame += payload;

        return frame;
    }

    // decode
    DecodeResult decode(const std::string& header,
                        const std::string& payload,
                        Message& out) {
        // 1.检查头长
        if (header.size() != HEADER_SIZE) {
            return DecodeResult::BadMagic;
        }
        // 2.解析 MAGIC
        uint16_t magic = (static_cast<uint8_t>(header[0]) << 8) |
                         static_cast<uint8_t>(header[1]);
        if (magic != MAGIC) {
            return DecodeResult::BadMagic;
        }
        // 3.解析 VERSION
        uint8_t version = static_cast<uint8_t>(header[2]);
        if (version != PROTOCOL_VERSION) {
            return DecodeResult::BadVersion;
        }
        // 4.解析 TYPE
        uint8_t type = static_cast<uint8_t>(header[3]);
        // 5.解析 LENGTH
        uint32_t length = (static_cast<uint8_t>(header[4]) << 24) |
                          (static_cast<uint8_t>(header[5]) << 16) |
                          (static_cast<uint8_t>(header[6]) << 8) |
                          static_cast<uint8_t>(header[7]);
        if (length > MAX_PROTOCOL_LENGTH) {
            return DecodeResult::LengthTooLarge;
        }
        if (length != payload.size()) {
            return DecodeResult::BadJson;
        }
        // 6.解析 JSON
        try {
            out.payload = nlohmann::json::parse(payload);
        } catch (const nlohmann::json::exception&) {
            return DecodeResult::BadJson;
        }
        // 7.填充类型
        out.type = type;

        return DecodeResult::Ok;
    }

    // read_message，从 fd 读一条完整消息
    ReadResult read_message(int fd, Message& out) {
        // 1.读取头部
        std::string header(HEADER_SIZE, '\0');
        if(!recv_exact(fd, header.data(), HEADER_SIZE)) {
            // recv_exact 返回 false：对端关闭或网络错误
            return ReadResult::Closed;
        }
        // 2.检查头部
        // MAGIC
        uint16_t magic = (static_cast<uint8_t>(header[0]) << 8) |
                        static_cast<uint8_t>(header[1]);
        if (magic != MAGIC) {
            return ReadResult::ProtocolError;
        }
        // VERSION
        uint8_t version = static_cast<uint8_t>(header[2]);
        if (version != PROTOCOL_VERSION) {
            return ReadResult::ProtocolError;
        }
        // LENGTH
        uint32_t length = (static_cast<uint8_t>(header[4]) << 24) |
                          (static_cast<uint8_t>(header[5]) << 16) |
                          (static_cast<uint8_t>(header[6]) << 8) |
                          static_cast<uint8_t>(header[7]);
        if (length > MAX_PROTOCOL_LENGTH) {
            return ReadResult::ProtocolError;
        }
        // 3.读负载
        std::string payload(length, '\0');
        if (!recv_exact(fd, payload.data(), length)) {
            return ReadResult::Closed;
        }
        // 4.调用 decode 解析
        Message msg;
        DecodeResult dr = decode(header, payload, msg);
        if (dr != DecodeResult::Ok) {
            return ReadResult::ProtocolError;
        }

        out = std::move(msg);
        return ReadResult::Ok;
    }

    // send_message
    bool send_message(int fd, const Message& msg) {
        // 编码
        std::string frame = encode(msg);
        if (frame.empty()) {
            return false;
        }
        // 发送
        return send_all(fd, frame.data(), frame.size());
    }

    // make_error
    Message make_error(int code, const std::string& message) {
        Message msg;
        msg.type = MSG_ERROR;
        msg.payload["code"] = code;
        msg.payload["message"] = message;
        return msg;
    }
} // namespace chatlab