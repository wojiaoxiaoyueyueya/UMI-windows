// DeviceManager.hpp - 设备管理器
// 负责检测海康/Orbbec相机和夹爪设备，分配左右槽位
// 支持热插拔重扫描

#pragma once

#include "ICamera.hpp"
#include "IGripper.hpp"
#include "Config.hpp"
#include <vector>
#include <map>
#include <string>
#include <memory>
#include <mutex>
#include <cstdint>

struct DetectedDevice {
    std::string type;       // "hikvision", "orbbec"
    std::string name;
    std::string serialNumber;
    int index;              // 设备在 SDK 列表中的索引
};

struct DetectedGripper {
    std::string type;       // "manual" or "electric"
    std::string port;       // COM port name
    bool connected = false;
};

struct GripperSlot {
    std::string position;       // "left", "right", or "extra"
    std::string gripperType;    // "manual", "electric", or "none"
    std::unique_ptr<IGripper> gripper;
    bool connected = false;

    GripperSlot() = default;
    GripperSlot(const std::string& pos) : position(pos), gripperType("none"), connected(false) {}
    GripperSlot(GripperSlot&&) = default;
    GripperSlot& operator=(GripperSlot&&) = default;
    GripperSlot(const GripperSlot&) = delete;
    GripperSlot& operator=(const GripperSlot&) = delete;
};

class DeviceManager {
public:
    explicit DeviceManager(const Config& cfg);
    ~DeviceManager();

    // 检测所有设备并分配槽位
    bool detectAll();

    // 重新检测相机并重建槽位。该接口会关闭已有相机，只适合初始化或明确重置时使用。
    bool reDetectCameras();

    // 只刷新相机检测列表，不关闭或重建当前正在采集的相机槽位。
    // 前端“重新扫描设备”使用该接口，避免扫描时打断主采集线程持有的相机对象。
    bool refreshDetectedCameras();
    // 将新检测到的相机补挂到当前仍为空的槽位，不影响已连接槽位。
    bool attachDetectedCamerasToEmptySlots();

    // 检测夹爪并重新分配左右夹爪槽位
    bool detectGrippers();

    // 只刷新夹爪检测列表，不重新打开串口或 CAN，供页面实时显示断开状态。
    bool refreshDetectedGrippers();
    // 将新检测到的夹爪补挂到当前仍为空的槽位，不影响已连接槽位。
    bool attachDetectedGrippersToEmptySlots(bool allowElectricScan = true);

    // 获取相机槽位
    DeviceSlot* getSlot(const std::string& position);
    const DeviceSlot* getSlot(const std::string& position) const;

    // 获取夹爪槽位
    GripperSlot* getGripperSlot(const std::string& position);
    const GripperSlot* getGripperSlot(const std::string& position) const;

    // 获取所有检测到的设备信息
    const std::vector<DetectedDevice>& getDetectedDevices() const { return detectedDevices_; }
    const std::vector<DetectedGripper>& getDetectedGrippers() const { return detectedGrippers_; }

    // 统计
    int getOrbbecCount() const;
    int getHikvisionCount() const;
    std::vector<std::string> getSlotNames() const;
    std::vector<std::string> getGripperSlotNames() const;

    // 序列化为 JSON（供 API 返回）
    std::string toJson() const;

    // 交换两个槽位的摄像头
    bool swapSlots(const std::string& pos1, const std::string& pos2);

    // 将指定序列号的摄像头分配到目标槽位
    bool assignCamera(const std::string& serial, const std::string& targetSlot);

private:
    void detectOrbbecDevices();
    void detectHikvisionDevices();
    void assignSlots();
    void detectAndAssignGrippers();

    Config cfg_;
    std::map<std::string, DeviceSlot> slots_;          // "left"/"right"/"head" -> camera slot
    std::map<std::string, GripperSlot> gripperSlots_;  // "left"/"right"/"extra" -> gripper slot
    std::vector<DetectedDevice> detectedDevices_;
    std::vector<DetectedGripper> detectedGrippers_;
    std::vector<std::unique_ptr<ICamera>> retiredCameras_;
    mutable std::mutex detectedInfoMutex_;
    uint64_t lastCameraRefreshUs_ = 0;
    uint64_t lastGripperRefreshUs_ = 0;
};
