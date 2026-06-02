// ICamera.hpp - 抽象相机接口
// 统一 Hikvision 工业相机和 Orbbec 深度相机的接口

#pragma once

#include <string>
#include <functional>
#include <memory>
#include <opencv2/opencv.hpp>

class ICamera {
public:
    virtual ~ICamera() = default;

    // 生命周期
    virtual bool open(int index = 0, const std::string& serial = "") = 0;
    virtual void close() = 0;
    virtual bool isOpened() const = 0;

    // 设备信息
    virtual std::string getDeviceName() const = 0;
    virtual std::string getSerialNumber() const = 0;
    virtual std::string getDeviceType() const = 0;   // "hikvision" or "orbbec"

    // 能力查询
    virtual bool hasColorStream() const = 0;
    virtual bool hasDepthStream() const = 0;
    virtual bool hasIRStream() const = 0;
    virtual bool hasIMU() const = 0;

    // 轮询式采集（Hikvision 用）
    virtual cv::Mat readColor() { return cv::Mat(); }

    // 回调式采集（Orbbec 用）
    using FrameCallback = std::function<void(const cv::Mat&, uint64_t timestampUs)>;
    using DepthFrameCallback = std::function<void(const cv::Mat& visualization, const cv::Mat& rawDepth, uint64_t timestampUs)>;
    using IRFrameCallback = std::function<void(const cv::Mat& irFrame, uint64_t timestampUs)>;
    using PointCloudCallback = std::function<void(const std::vector<float>& points, int width, int height, uint64_t timestampUs)>;

    virtual void setColorCallback(FrameCallback cb) { colorCb_ = cb; }
    virtual void setDepthCallback(DepthFrameCallback cb) { depthCb_ = cb; }
    virtual void setIRLeftCallback(IRFrameCallback cb) { irLeftCb_ = cb; }
    virtual void setIRRightCallback(IRFrameCallback cb) { irRightCb_ = cb; }
    virtual void setPointCloudCallback(PointCloudCallback cb) { pointCloudCb_ = cb; }

    // 启停流（Orbbec 需要 Pipeline start/stop）
    virtual bool startStreaming() { return true; }
    virtual void stopStreaming() {}

    // 相机内参（SLAM 用）
    virtual bool getIntrinsics(float& fx, float& fy, float& cx, float& cy) const { return false; }

protected:
    FrameCallback colorCb_;
    DepthFrameCallback depthCb_;
    IRFrameCallback irLeftCb_;
    IRFrameCallback irRightCb_;
    PointCloudCallback pointCloudCb_;
};

// 设备槽位
struct DeviceSlot {
    std::string position;       // "left" or "right"
    std::string deviceType;     // "hikvision", "orbbec", or "none"
    std::unique_ptr<ICamera> camera;
    bool connected = false;

    DeviceSlot() = default;
    DeviceSlot(const std::string& pos) : position(pos), deviceType("none"), connected(false) {}
};
