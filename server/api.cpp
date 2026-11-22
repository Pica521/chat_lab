#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include "server/api.h"
#include "server/state.h"
// 该文件处理注册、登录等业务

using json = nlohmann::json;

// 生成 JSON 格式响应
static std::string make_json_response(int code, const std::string& msg) {
    json resp;
    resp["code"] = code;
    resp["msg"] = msg;
    return resp.dump();
}

// 处理注册逻辑
std::string handle_api_register(const std::string& body, AppState& state) {
    try{
        // 1. 解析前端发来的 JSON 数据(请求)
        json req = json::parse(body);
        std::string username = req.value("username", "");
        std::string password = req.value("password", "");

        // 2. 长度检查
        if (username.empty() || password.empty()) {
            return make_json_response(400, "用户名和密码不能为空");
        }
        if (username.size() > 32) {
            return make_json_response(400, "用户名过长 (至多32字符)");
        }

        // 3. 查重与写入，同一把锁内完成，避免并发注册通过查重
        {
            std::lock_guard<std::mutex> lock(state.mtx);
            if (state.users.find(username) != state.users.end()) {
                return make_json_response(400, "该用户名已被注册");
            }
            state.users[username] = password;
        }

        std::cout << "[API] New user registered: " << username << std::endl;
        return make_json_response(200, "注册成功，请登录");
    } catch (const json::exception& e) {
        // 捕获异常(请求格式不合法), 避免服务器崩溃
        std::cerr << "[API] JSON parse error in register: " << e.what() << std::endl;
        return make_json_response(400, "无效的请求格式");
    }
}

// 处理登录逻辑
std::string handle_api_login(const std::string& body, AppState& state) {
    try {
        // 1. 解析请求
        json req = json::parse(body);
        std::string username = req.value("username", "");
        std::string password = req.value("password", "");

        if (username.empty() || password.empty()) {
            return make_json_response(400, "用户名和密码不能为空");
        }

        // 2. 加锁查找用户, 校验密码
        {
            std::lock_guard<std::mutex> lock(state.mtx);
            auto it = state.users.find(username);
            if (it == state.users.end()) {
                return make_json_response(400, "用户不存在");
            }
            if (it->second != password) {
                return make_json_response(400, "密码错误");
            }
        }
        
        // 3. 登陆成功
        std::cout << "[API] User logged in: " << username << std::endl;
        return make_json_response(200, "登陆成功");
    } catch (const json::exception& e) {
        std::cerr << "[API] JSON parse error in login: " << e.what() << std::endl;
        return make_json_response(400, "无效的请求格式");
    }
}