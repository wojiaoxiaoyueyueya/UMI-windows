// Config.cpp - 配置文件读取实现

#include "Config.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#ifndef _WIN32
#include <unistd.h>
#else
#include <windows.h>
#endif

// 简易 JSON 解析（无第三方依赖）
static std::string readWholeFile(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string extractString(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos);
    if (pos == std::string::npos) return "";
    size_t end = json.find('"', pos + 1);
    if (end == std::string::npos) return "";
    return json.substr(pos + 1, end - pos - 1);
}

static double extractNumber(const std::string& json, const std::string& key, double defaultVal) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return defaultVal;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return defaultVal;
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    if (pos >= json.size()) return defaultVal;
    char* end;
    double val = strtod(json.c_str() + pos, &end);
    if (end == json.c_str() + pos) return defaultVal;
    return val;
}

Config Config::load(const std::string& exeDir) {
    Config cfg;

    std::string configPath = exeDir + "/../config.json";
    std::string content = readWholeFile(configPath);
    if (content.empty()) {
        fprintf(stderr, "[配置] 未找到 %s，使用默认配置\n", configPath.c_str());
        return cfg;
    }

    // 服务端口配置：控制本地 HTTP 服务监听端口。
    cfg.server.port = (int)extractNumber(content, "port", cfg.server.port);

    // 路径配置：控制前端目录、原始数据目录、转换脚本和转换输出目录。
    cfg.paths.frontendDir = extractString(content, "frontendDir");
    if (cfg.paths.frontendDir.empty()) cfg.paths.frontendDir = "frontend";
    cfg.paths.dataDir = extractString(content, "dataDir");
    if (cfg.paths.dataDir.empty()) cfg.paths.dataDir = "data_capture";
    cfg.paths.convertOutputDir = extractString(content, "convertOutputDir");
    if (cfg.paths.convertOutputDir.empty()) cfg.paths.convertOutputDir = "data_converted";
    cfg.paths.convertScript = extractString(content, "convertScript");
    if (cfg.paths.convertScript.empty()) cfg.paths.convertScript = "tools/convert_to_lerobot.py";

    // 相机配置：控制海康索引、序列号偏好、鱼眼畸变和图像亮度等参数。
    cfg.camera.fisheyeK1 = extractNumber(content, "fisheyeK1", cfg.camera.fisheyeK1);
    cfg.camera.fisheyeK2 = extractNumber(content, "fisheyeK2", cfg.camera.fisheyeK2);
    cfg.camera.fisheyeScale = extractNumber(content, "fisheyeScale", cfg.camera.fisheyeScale);
    cfg.camera.brightness = extractNumber(content, "brightness", cfg.camera.brightness);
    cfg.camera.contrast = extractNumber(content, "contrast", cfg.camera.contrast);
    cfg.camera.maxWidth = (int)extractNumber(content, "maxWidth", cfg.camera.maxWidth);
    cfg.camera.maxHeight = (int)extractNumber(content, "maxHeight", cfg.camera.maxHeight);
    cfg.camera.fps = (int)extractNumber(content, "fps", cfg.camera.fps);

    // 推流配置：控制编码间隔、MJPEG 发送间隔、JPEG 质量和预览宽度。
    cfg.stream.encodeIntervalMs = (int)extractNumber(content, "encodeIntervalMs", cfg.stream.encodeIntervalMs);
    cfg.stream.streamIntervalMs = (int)extractNumber(content, "streamIntervalMs", cfg.stream.streamIntervalMs);
    cfg.stream.jpegQuality = (int)extractNumber(content, "jpegQuality", cfg.stream.jpegQuality);
    cfg.stream.streamMaxWidth = (int)extractNumber(content, "streamMaxWidth", cfg.stream.streamMaxWidth);
    cfg.stream.frameSkipMs = (int)extractNumber(content, "frameSkipMs", cfg.stream.frameSkipMs);

    // SLAM 配置：控制是否启用位姿估计及其关键参数。
    {
        size_t slamPos = content.find("\"slam\"");
        if (slamPos != std::string::npos) {
            size_t blockStart = content.find('{', slamPos);
            size_t blockEnd = content.find('}', blockStart);
            if (blockStart != std::string::npos && blockEnd != std::string::npos) {
                std::string block = content.substr(blockStart, blockEnd - blockStart + 1);
                cfg.slam.enabled = (int)extractNumber(block, "enabled", cfg.slam.enabled ? 1 : 0) != 0;
                cfg.slam.depthScale = extractNumber(block, "depthScale", cfg.slam.depthScale);
            }
        }
    }

    // 多设备配置：读取 left/right/head 等槽位的设备偏好。
    {
        size_t devPos = content.find("\"devices\"");
        if (devPos != std::string::npos) {
            // 提取 left 槽位配置。
            size_t leftPos = content.find("\"left\"", devPos);
            if (leftPos != std::string::npos) {
                DeviceSlotConfig leftCfg;
                size_t blockStart = content.find('{', leftPos);
                size_t blockEnd = content.find('}', blockStart);
                if (blockStart != std::string::npos && blockEnd != std::string::npos) {
                    std::string block = content.substr(blockStart, blockEnd - blockStart + 1);
                    leftCfg.preferredType = extractString(block, "preferredType");
                    if (leftCfg.preferredType.empty()) leftCfg.preferredType = "auto";
                    leftCfg.serialHint = extractString(block, "serialHint");
                }
                cfg.devices["left"] = leftCfg;
            }

            // 提取 right 槽位配置。
            size_t rightPos = content.find("\"right\"", devPos);
            if (rightPos != std::string::npos) {
                DeviceSlotConfig rightCfg;
                size_t blockStart = content.find('{', rightPos);
                size_t blockEnd = content.find('}', blockStart);
                if (blockStart != std::string::npos && blockEnd != std::string::npos) {
                    std::string block = content.substr(blockStart, blockEnd - blockStart + 1);
                    rightCfg.preferredType = extractString(block, "preferredType");
                    if (rightCfg.preferredType.empty()) rightCfg.preferredType = "auto";
                    rightCfg.serialHint = extractString(block, "serialHint");
                }
                cfg.devices["right"] = rightCfg;
            }
        }

        // 向后兼容：旧配置没有 devices 字段时，创建默认左右手槽位。
        if (cfg.devices.empty()) {
            DeviceSlotConfig leftCfg;
            leftCfg.preferredType = "auto";
            cfg.devices["left"] = leftCfg;
            cfg.devices["right"] = leftCfg;
        }
    }

    fprintf(stderr, "[配置] 已加载 %s\n", configPath.c_str());
    fprintf(stderr, "[配置] 端口=%d, 数据目录=%s, 鱼眼K1=%.2f, 亮度=%.1f, SLAM=%d\n",
        cfg.server.port, cfg.paths.dataDir.c_str(), cfg.camera.fisheyeK1, cfg.camera.brightness, cfg.slam.enabled);
    return cfg;
}
