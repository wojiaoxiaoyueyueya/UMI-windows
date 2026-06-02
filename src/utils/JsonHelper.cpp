// JsonHelper.cpp - 简易 JSON 解析工具函数实现
#include "utils/JsonHelper.hpp"
#include "httplib.h"

namespace json {

std::string escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;
        }
    }
    return out;
}

void sendJson(httplib::Response& res, const std::string& json) {
    res.set_content(json, "application/json");
    res.set_header("Access-Control-Allow-Origin", "*");
}

std::string extractStr(const std::string& body, const std::string& key) {
    auto p = body.find("\"" + key + "\"");
    if (p == std::string::npos) return "";
    auto c = body.find(':', p);
    if (c == std::string::npos) return "";
    auto sq = body.find('"', c);
    if (sq == std::string::npos) return "";
    auto eq = body.find('"', sq + 1);
    if (eq == std::string::npos) return "";
    return body.substr(sq + 1, eq - sq - 1);
}

int extractInt(const std::string& body, const std::string& key, int defaultVal) {
    auto p = body.find("\"" + key + "\"");
    if (p == std::string::npos) return defaultVal;
    auto c = body.find(':', p);
    if (c == std::string::npos) return defaultVal;
    size_t numStart = c + 1;
    while (numStart < body.size() && (body[numStart] == ' ' || body[numStart] == '\t')) numStart++;
    size_t numEnd = numStart;
    while (numEnd < body.size() && body[numEnd] >= '0' && body[numEnd] <= '9') numEnd++;
    if (numEnd == numStart) return defaultVal;
    return std::stoi(body.substr(numStart, numEnd - numStart));
}

float extractFloat(const std::string& body, const std::string& key, float defaultVal) {
    auto p = body.find("\"" + key + "\"");
    if (p == std::string::npos) return defaultVal;
    auto c = body.find(':', p);
    if (c == std::string::npos) return defaultVal;
    size_t numStart = c + 1;
    while (numStart < body.size() && (body[numStart] == ' ' || body[numStart] == '\t')) numStart++;
    size_t numEnd = numStart;
    while (numEnd < body.size() && body[numEnd] != ',' && body[numEnd] != '}' && body[numEnd] != '"') numEnd++;
    try { return std::stof(body.substr(numStart, numEnd - numStart)); }
    catch (...) { return defaultVal; }
}

std::vector<std::string> extractStringArray(const std::string& body, const std::string& key) {
    std::vector<std::string> result;
    auto pos = body.find("\"" + key + "\"");
    if (pos == std::string::npos) return result;
    auto bs = body.find('[', pos);
    if (bs == std::string::npos) return result;
    auto be = body.find(']', bs);
    if (be == std::string::npos) return result;
    std::string arr = body.substr(bs + 1, be - bs - 1);
    size_t tp = 0;
    while ((tp = arr.find('"', tp)) != std::string::npos) {
        auto te = arr.find('"', tp + 1);
        if (te != std::string::npos) {
            result.push_back(arr.substr(tp + 1, te - tp - 1));
            tp = te + 1;
        } else break;
    }
    return result;
}

std::map<std::string, std::string> extractStringMap(const std::string& body, const std::string& key) {
    std::map<std::string, std::string> result;
    auto pos = body.find("\"" + key + "\"");
    if (pos == std::string::npos) return result;
    auto braceStart = body.find('{', pos);
    if (braceStart == std::string::npos) return result;
    auto braceEnd = body.find('}', braceStart);
    if (braceEnd == std::string::npos) return result;
    std::string mappingStr = body.substr(braceStart + 1, braceEnd - braceStart - 1);
    size_t p = 0;
    while (p < mappingStr.size()) {
        auto kq1 = mappingStr.find('"', p);
        if (kq1 == std::string::npos) break;
        auto kq2 = mappingStr.find('"', kq1 + 1);
        if (kq2 == std::string::npos) break;
        std::string k = mappingStr.substr(kq1 + 1, kq2 - kq1 - 1);
        auto colon = mappingStr.find(':', kq2);
        if (colon == std::string::npos) break;
        auto vq1 = mappingStr.find('"', colon);
        if (vq1 == std::string::npos) break;
        auto vq2 = mappingStr.find('"', vq1 + 1);
        if (vq2 == std::string::npos) break;
        result[k] = mappingStr.substr(vq1 + 1, vq2 - vq1 - 1);
        p = vq2 + 1;
    }
    return result;
}

} // namespace json
