// JsonHelper.hpp - 简易 JSON 解析工具函数（无第三方依赖）
#pragma once

#include <string>
#include <vector>
#include <map>
#include <cstdlib>

namespace httplib { class Response; }

namespace json {

std::string escape(const std::string& s);
void sendJson(httplib::Response& res, const std::string& json);

std::string extractStr(const std::string& body, const std::string& key);
int extractInt(const std::string& body, const std::string& key, int defaultVal = -1);
float extractFloat(const std::string& body, const std::string& key, float defaultVal = 0.0f);

// 提取 JSON 数组中的字符串列表，如 ["a","b","c"]
std::vector<std::string> extractStringArray(const std::string& body, const std::string& key);

// 提取 JSON 对象中的字符串键值对，如 {"k1":"v1","k2":"v2"}
std::map<std::string, std::string> extractStringMap(const std::string& body, const std::string& key);

} // namespace json
