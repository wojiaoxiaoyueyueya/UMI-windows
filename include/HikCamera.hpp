// HikCamera.hpp - 海康工业相机采集封装

#pragma once

#include <string>
#include <atomic>
#include <memory>
#include <opencv2/opencv.hpp>
#include "Config.hpp"

typedef void* voidP;

class HikCamera {
public:
    HikCamera(const CameraConfig& cfg);
    ~HikCamera();

    bool open(int index = 0, const std::string& serial = "");
    void close();
    bool isOpened() const;

    cv::Mat read();

    std::string getDeviceName() const { return deviceName_; }
    std::string getSerialNumber() const { return serialNumber_; }

private:
    void* handle_;
    std::atomic<bool> opened_;
    std::string deviceName_;
    std::string serialNumber_;
    CameraConfig cfg_;
    int deviceIndex_ = 0;
    cv::Mat map1_;
    cv::Mat map2_;
    int mapW_ = 0;
    int mapH_ = 0;
    double mapK1_ = 0.0;
    double mapK2_ = 0.0;
    double mapScale_ = 0.0;
    unsigned int lastPixelType_ = 0;

public:
    // 每个实例独立的帧缓冲区（定义在 .cpp 中，需被全局回调访问）
    struct RawFrameBuffer;
    std::unique_ptr<RawFrameBuffer> rawFrame_;
};
