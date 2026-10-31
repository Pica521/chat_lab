#pragma once
#include <string>
#include "server/http.h"

// 处理业务逻辑，返回响应的 Body (JSON)
std::string handle_api_register(const std::string& body);
std::string handle_api_login(const std::string& body);