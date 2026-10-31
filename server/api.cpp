#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include "server/api.h"

using json = nlohmann::json;
// 全局用户表，保存 用户名 + 密码
static std::map<std::string, std::string> g_users;

// 生成 JSON 格式响应
static std::string make_json_response(int code, const std::string& msg) {
    json resp;
    resp["code"] = code;
    resp["msg"] = msg;
    return resp.dump();
}

// 处理注册逻辑
std::string handle_api_register(const std::string& body) {
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

        // 3. 查重
        if (g_users.find(username) != g_users.end()) {
            return make_json_response(400, "该用户名已被注册");
        }

        // 4. 注册成功，写入全局表
        g_users[username] = password;
        std::cout << "[API] New user registered: " << username << std::endl;
        return make_json_response(200, "注册成功，请登录");
    } catch (const json::exception& e) {
        // 捕获异常(请求格式不合法), 避免服务器崩溃
        std::cerr << "[API] JSON parse error in register: " << e.what() << std::endl;
        return make_json_response(400, "无效的请求格式");
    }
}

// 处理登录逻辑
std::string handle_api_login(const std::string& body) {
    try {
        // 1. 解析请求
        json req = json::parse(body);
        std::string username = req.value("username", "");
        std::string password = req.value("password", "");

        if (username.empty() || password.empty()) {
            return make_json_response(400, "用户名和密码不能为空");
        }

        // 2. 查找用户, 校验密码
        auto it = g_users.find(username);
        if (it == g_users.end()) {
            return make_json_response(400, "用户不存在");
        }
        if (it->second != password) {
            return make_json_response(400, "密码错误");
        }

        // 3. 登陆成功
        std::cout << "[API] User logged in: " << username << std::endl;
        
        return make_json_response(200, "登陆成功");
    } catch (const json::exception& e) {
        std::cerr << "[API] JSON parse error in login: " << e.what() << std::endl;
        return make_json_response(400, "无效的请求格式");
    }
}