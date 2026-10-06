#include "common/protocol.h"

#include <cassert>
#include <iostream>
#include <string>

using namespace chatlab;

// 测试计数
static int g_passed = 0;
static int g_failed = 0;

// 断言宏：失败时打印文件和行号，不直接 abort
#define CHECK(cond, msg)                                            \
    do {                                                            \
        if (cond) {                                                 \
            ++g_passed;                                             \
        } else {                                                    \
            ++g_failed;                                             \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__   \
                      << " " << msg << std::endl;                   \
        }                                                           \
    } while (0)

// 测试函数
// 1.编解码往返
void test_encode_decode_roundtrip() {
    Message original;
    original.type = MSG_LOGIN;
    original.payload["username"] = "alice";

    std::string frame = encode(original);
    CHECK(!frame.empty(), "encode should not return empty");

    // 拆出头部和负载
    std::string header = frame.substr(0, HEADER_SIZE);
    std::string payload = frame.substr(HEADER_SIZE);

    Message decoded;
    DecodeResult dr = decode(header, payload, decoded);
    CHECK(dr == DecodeResult::Ok, "decode should return Ok");
    CHECK(decoded.type == MSG_LOGIN, "type should match");
    CHECK(decoded.payload["username"] == "alice", "username should match");
}
// 2.MAGIC 篡改
void test_bad_magic() {
    Message msg;
    msg.type = MSG_LOGIN;
    msg.payload["username"] = "alice";
    std::string frame = encode(msg);

    // 篡改 MAGIC 第一个字节
    frame[0] = static_cast<char>(0xFF);

    std::string header = frame.substr(0, HEADER_SIZE);
    std::string payload = frame.substr(HEADER_SIZE);
    Message decoded;
    CHECK(decode(header, payload, decoded) == DecodeResult::BadMagic,
          "should detect bad magic");
}
// 3.VERSION 篡改
void test_bad_version() {
    Message msg;
    msg.type = MSG_LOGIN;
    msg.payload["username"] = "alice";
    std::string frame = encode(msg);

    // 篡改 VERSION
    frame[2] = static_cast<char>(0xFF);

    std::string header = frame.substr(0, HEADER_SIZE);
    std::string payload = frame.substr(HEADER_SIZE);
    Message decoded;
    CHECK(decode(header, payload, decoded) == DecodeResult::BadVersion,
          "should detect bad version");
}
// 4.LENGTH 超限
void test_length_too_large() {
    // 构造头部，MAGIC 和 VERSION 合法，LENGTH 填 5 MiB
    std::string header(HEADER_SIZE, '\0');
    header[0] = 0x43;
    header[1] = 0x4C;
    header[2] = 0x01;
    header[3] = MSG_LOGIN;
    uint32_t big = 5 * 1024 * 1024;
    header[4] = static_cast<char>((big >> 24) & 0xFF);
    header[5] = static_cast<char>((big >> 16) & 0xFF);
    header[6] = static_cast<char>((big >> 8) & 0xFF);
    header[7] = static_cast<char>(big & 0xFF);

    std::string payload;  // 空
    Message decoded;
    CHECK(decode(header, payload, decoded) == DecodeResult::LengthTooLarge,
          "should detect length too large");
}
// 5.非法 JSON
void test_bad_json() {
    Message msg;
    msg.type = MSG_LOGIN;
    msg.payload["username"] = "alice";
    std::string frame = encode(msg);

    // 把 payload 替换成非法 JSON
    std::string header = frame.substr(0, HEADER_SIZE);
    std::string payload = "this is not json";

    // 同步更新 LENGTH 字段，以防被长度检查拦截
    uint32_t len = static_cast<uint32_t>(payload.size());
    header[4] = static_cast<char>((len >> 24) & 0xFF);
    header[5] = static_cast<char>((len >> 16) & 0xFF);
    header[6] = static_cast<char>((len >> 8) & 0xFF);
    header[7] = static_cast<char>(len & 0xFF);

    Message decoded;
    CHECK(decode(header, payload, decoded) == DecodeResult::BadJson,
          "should detect bad json");
}
// 6.空 payload
void test_empty_payload() {
    Message msg;
    msg.type = MSG_LIST;
    msg.payload = nlohmann::json::object();

    std::string frame = encode(msg);
    std::string header = frame.substr(0, HEADER_SIZE);
    std::string payload = frame.substr(HEADER_SIZE);

    Message decoded;
    CHECK(decode(header, payload, decoded) == DecodeResult::Ok,
          "empty payload should decode");
    CHECK(decoded.type == MSG_LIST, "type should match");
}
// 7.1MiB 消息
void test_large_message() {
    Message msg;
    msg.type = MSG_BROADCAST;
    msg.payload["content"] = std::string(1 * 1024 * 1024, 'A');

    std::string frame = encode(msg);
    CHECK(!frame.empty(), "1 MiB message should encode");

    std::string header = frame.substr(0, HEADER_SIZE);
    std::string payload = frame.substr(HEADER_SIZE);

    Message decoded;
    CHECK(decode(header, payload, decoded) == DecodeResult::Ok,
          "1 MiB message should decode");
    CHECK(decoded.payload["content"].get<std::string>().size()
          == 1 * 1024 * 1024, "content size should match");
}
// make_error 检查
void test_make_error() {
    Message err = make_error(ERR_USER_NOT_ONLINE, "user offline");
    CHECK(err.type == MSG_ERROR, "type should be MSG_ERROR");
    CHECK(err.payload["code"] == ERR_USER_NOT_ONLINE, "code should match");
    CHECK(err.payload["message"] == "user offline", "message should match");
}

// 主函数
int main() {

    test_encode_decode_roundtrip();
    test_bad_magic();
    test_bad_version();
    test_length_too_large();
    test_bad_json();
    test_empty_payload();
    test_large_message();
    test_make_error();

    std::cout << "Passed: " << g_passed
              << ", Failed: " << g_failed << std::endl;
    return g_failed == 0 ? 0 : 1;
}