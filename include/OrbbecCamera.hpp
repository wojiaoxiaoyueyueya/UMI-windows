// OrbbecCamera.hpp - Orbbec Gemini 305 深度相机实现
// 使用 OrbbecSDK v2 C API，支持 Color/Depth/IR 流

#pragma once

#include "ICamera.hpp"
#include <atomic>
#include <mutex>
#include <thread>

#ifndef NO_ORBBEC_CAMERA

// 使用 SDK 头文件中的类型定义
#include <libobsensor/ObSensor.h>

class OrbbecCamera : public ICamera {
public:
    OrbbecCamera();
    ~OrbbecCamera() override;

    bool open(int deviceIndex = 0, const std::string& serial = "") override;
    void close() override;
    bool isOpened() const override;

    std::string getDeviceName() const override;
    std::string getSerialNumber() const override;
    std::string getDeviceType() const override { return "orbbec"; }

    bool hasColorStream() const override { return hasColor_; }
    bool hasDepthStream() const override { return hasDepth_; }
    bool hasIRStream() const override { return hasIR_; }
    bool hasIMU() const override { return false; }

    bool startStreaming() override;
    void stopStreaming() override;

    bool getIntrinsics(float& fx, float& fy, float& cx, float& cy) const override;

private:
    void initCapabilities();
    void onFrameSetCallback(ob_frame* frameset);
    static void frameSetCallback(ob_frame* frameset, void* userdata);

    ob_context*  context_ = nullptr;
    ob_pipeline* pipeline_ = nullptr;
    ob_config*   config_ = nullptr;
    ob_device*   device_ = nullptr;

    // 点云 filter
    ob_filter* pointCloudFilter_ = nullptr;
    ob_filter* alignFilter_ = nullptr;

    std::atomic<bool> opened_{false};
    std::atomic<bool> streaming_{false};
    std::string deviceName_;
    std::string serialNumber_;

    bool hasColor_ = false;
    bool hasDepth_ = false;
    bool hasIR_ = false;
    bool hasIMU_ = false;

    float fx_ = 0, fy_ = 0, cx_ = 0, cy_ = 0;
    bool hasIntrinsics_ = false;

    // 帧格式转换
    static cv::Mat convertColorFrame(ob_frame* frame);
    static cv::Mat convertDepthFrame(ob_frame* frame);
    static cv::Mat convertIRFrame(ob_frame* frame);
};

#else

// 无 Orbbec SDK 时的空实现
class OrbbecCamera : public ICamera {
public:
    OrbbecCamera() {}
    ~OrbbecCamera() override {}
    bool open(int = 0, const std::string& = "") override { return false; }
    void close() override {}
    bool isOpened() const override { return false; }
    std::string getDeviceName() const override { return "Orbbec (SDK not available)"; }
    std::string getSerialNumber() const override { return "N/A"; }
    std::string getDeviceType() const override { return "orbbec"; }
    bool hasColorStream() const override { return false; }
    bool hasDepthStream() const override { return false; }
    bool hasIRStream() const override { return false; }
    bool startStreaming() override { return false; }
    void stopStreaming() override {}
};

#endif // NO_ORBBEC_CAMERA
