// Config.hpp - 项目配置管理（多设备版）
// 从 config.json 读取配置，不存在则使用默认值

#pragma once

#include <string>
#include <map>
#include <vector>

struct CameraConfig {
    double fisheyeK1 = -1.0;
    double fisheyeK2 = 0.4;
    double fisheyeScale = 1.5;
    double brightness = 15.0;
    double contrast = 1.1;
    int maxWidth = 0;
    int maxHeight = 0;
    int fps = 30;
};

struct OrbbecStreamConfig {
    int colorWidth = 1280;
    int colorHeight = 800;
    int depthWidth = 1280;
    int depthHeight = 800;
    int fps = 30;
};

struct DeviceSlotConfig {
    std::string type = "auto";           // "auto", "hikvision", "orbbec"
    std::string preferredType = "auto";  // 优先设备类型
    std::string serialHint;              // 序列号匹配
    CameraConfig hikvision;
    OrbbecStreamConfig orbbec;
};

struct StreamConfig {
    int encodeIntervalMs = 30;
    int streamIntervalMs = 33;
    int jpegQuality = 95;
    int streamMaxWidth = 640;
    int frameSkipMs = 33;
};

struct PathsConfig {
    std::string frontendDir = "frontend";
    std::string dataDir = "data_capture";
    std::string convertOutputDir = "data_converted";
    std::string convertScript = "tools/convert_to_lerobot.py";
};

struct ServerConfig {
    int port = 8080;
};

struct SlamConfig {
    bool enabled = true;
    double depthScale = 0.001;
};

struct Config {
    ServerConfig server;
    PathsConfig paths;
    CameraConfig camera;  // 向后兼容
    StreamConfig stream;
    SlamConfig slam;
    std::map<std::string, DeviceSlotConfig> devices;  // "left"/"right" -> config

    static Config load(const std::string& exeDir);
};
