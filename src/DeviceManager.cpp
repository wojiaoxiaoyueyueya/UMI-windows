// DeviceManager.cpp - 设备管理器实现
// 检测海康/Orbbec相机和夹爪，分配左右槽位

#include "DeviceManager.hpp"
#include "HikCameraAdapter.hpp"
#include <set>
#include "OrbbecCamera.hpp"
#include "UmiGripper.hpp"
#include "ElectricGripper.hpp"
#include "ECanVciWrapper.hpp"

#include <cstdio>
#include <algorithm>
#include <chrono>

#ifndef NO_ORBBEC_CAMERA
#include <libobsensor/ObSensor.h>
#include <libobsensor/h/Context.h>
#include <libobsensor/h/Device.h>
#include <libobsensor/h/Error.h>

#else
#include <libobsensor/ObSensor.h>
#include <libobsensor/Context.h>
#include <libobsensor/Device.h>
#include <libobsensor/Error.h>
#endif

#ifndef NO_HIK_CAMERA
#include "MvCameraControl.h"
#endif

// ---- 构造/析构 ----

DeviceManager::DeviceManager(const Config& cfg) : cfg_(cfg) {
    slots_["left"]  = DeviceSlot("left");
    slots_["right"] = DeviceSlot("right");
    slots_["head"]  = DeviceSlot("head");
    gripperSlots_["left"]  = GripperSlot("left");
    gripperSlots_["right"] = GripperSlot("right");
    gripperSlots_["extra"] = GripperSlot("extra");
}

DeviceManager::~DeviceManager() = default;

// ---- 设备检测 ----

bool DeviceManager::detectAll() {
    detectedDevices_.clear();

    fprintf(stderr, "[DeviceManager] 开始检测设备...\n");

    detectOrbbecDevices();
    detectHikvisionDevices();

    fprintf(stderr, "[DeviceManager] 检测到 %d 个相机设备 (%d Orbbec, %d Hikvision)\n",
            (int)detectedDevices_.size(), getOrbbecCount(), getHikvisionCount());

    assignSlots();
    detectAndAssignGrippers();
    return !detectedDevices_.empty();
}

bool DeviceManager::reDetectCameras() {
    fprintf(stderr, "[DeviceManager] 重新检测相机...\n");

    // 停止现有流
    for (auto& kv : slots_) {
        if (kv.second.connected && kv.second.camera) {
            kv.second.camera->stopStreaming();
            kv.second.camera->close();
        }
        kv.second.connected = false;
        kv.second.camera.reset();
        kv.second.deviceType = "none";
    }

    detectedDevices_.clear();
    detectOrbbecDevices();
    detectHikvisionDevices();
    assignSlots();
    return !detectedDevices_.empty();
}

bool DeviceManager::refreshDetectedCameras() {
    std::lock_guard<std::mutex> lock(detectedInfoMutex_);
    uint64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (lastCameraRefreshUs_ > 0 && nowUs > lastCameraRefreshUs_
        && (nowUs - lastCameraRefreshUs_) < 3000000ULL) {
        return !detectedDevices_.empty();
    }
    lastCameraRefreshUs_ = nowUs;
    fprintf(stderr, "[DeviceManager] 刷新相机检测列表（不重建槽位）...\n");

    // 这里只更新检测列表，不关闭 slots_ 中已经打开的相机对象。
    // 主采集线程在服务启动时会持有相机指针；如果扫描按钮直接重置这些对象，
    // 页面会表现为所有设备断开，部分 SDK 还需要重启服务才能恢复。
    detectedDevices_.clear();
    detectOrbbecDevices();
    detectHikvisionDevices();

    std::set<std::string> detectedSerials;
    for (const auto& d : detectedDevices_) {
        if (!d.serialNumber.empty()) detectedSerials.insert(d.serialNumber);
    }
    for (auto& kv : slots_) {
        auto& slot = kv.second;
        if (!slot.connected || !slot.camera) continue;
        std::string slotSerial = slot.camera->getSerialNumber();
        if (!slotSerial.empty() && detectedSerials.count(slotSerial) > 0) continue;

        fprintf(stderr, "[DeviceManager] %s slot camera is no longer detected, releasing slot (SN=%s)\n",
                kv.first.c_str(), slotSerial.c_str());
        slot.camera->stopStreaming();
        slot.camera->close();
        retiredCameras_.push_back(std::move(slot.camera));
        slot.connected = false;
        slot.deviceType = "none";
    }

    fprintf(stderr, "[DeviceManager] 当前检测到 %d 个相机设备 (%d Orbbec, %d Hikvision)\n",
            (int)detectedDevices_.size(), getOrbbecCount(), getHikvisionCount());
    return !detectedDevices_.empty();
}

bool DeviceManager::attachDetectedCamerasToEmptySlots() {
    std::lock_guard<std::mutex> lock(detectedInfoMutex_);

    std::set<std::string> attachedSerials;
    for (const auto& kv : slots_) {
        if (kv.second.connected && kv.second.camera) {
            attachedSerials.insert(kv.second.camera->getSerialNumber());
        }
    }

    std::vector<DetectedDevice*> orbbecDevs;
    std::vector<DetectedDevice*> hikDevs;
    for (auto& d : detectedDevices_) {
        if (!d.serialNumber.empty() && attachedSerials.count(d.serialNumber) > 0) continue;
        if (d.type == "orbbec") orbbecDevs.push_back(&d);
        else if (d.type == "hikvision") hikDevs.push_back(&d);
    }

    auto assignSlot = [&](const std::string& slotName, DetectedDevice* dev) -> bool {
        auto& slot = slots_[slotName];
        if (slot.connected || !dev) return false;

        slot.deviceType = dev->type;
        slot.connected = false;

        if (dev->type == "orbbec") {
#ifndef NO_ORBBEC_CAMERA
            auto cam = std::make_unique<OrbbecCamera>();
            if (cam->open(dev->index, dev->serialNumber)) {
                slot.camera = std::move(cam);
                slot.connected = true;
                fprintf(stderr, "[DeviceManager] 热插拔补挂 %s 槽: Orbbec %s (SN: %s)\n",
                        slotName.c_str(), dev->name.c_str(), slot.camera->getSerialNumber().c_str());
                attachedSerials.insert(slot.camera->getSerialNumber());
                return true;
            }
#endif
        } else if (dev->type == "hikvision") {
            auto cam = std::make_unique<HikCameraAdapter>(cfg_.camera);
            if (cam->open(dev->index, dev->serialNumber)) {
                slot.camera = std::move(cam);
                slot.connected = true;
                fprintf(stderr, "[DeviceManager] 热插拔补挂 %s 槽: Hikvision %s (SN: %s)\n",
                        slotName.c_str(), dev->name.c_str(), slot.camera->getSerialNumber().c_str());
                attachedSerials.insert(slot.camera->getSerialNumber());
                return true;
            }
        }

        slot.deviceType = "none";
        slot.camera.reset();
        slot.connected = false;
        return false;
    };

    std::string leftPreferred  = cfg_.devices.count("left")  ? cfg_.devices.at("left").preferredType  : "auto";
    std::string rightPreferred = cfg_.devices.count("right") ? cfg_.devices.at("right").preferredType : "auto";
    bool changed = false;

    if (!slots_["left"].connected) {
        if (leftPreferred == "orbbec" && !orbbecDevs.empty()) {
            changed |= assignSlot("left", orbbecDevs.front());
            orbbecDevs.erase(orbbecDevs.begin());
        } else if (leftPreferred == "hikvision" && !hikDevs.empty()) {
            changed |= assignSlot("left", hikDevs.front());
            hikDevs.erase(hikDevs.begin());
        }
    }
    if (!slots_["right"].connected) {
        if (rightPreferred == "orbbec" && !orbbecDevs.empty()) {
            changed |= assignSlot("right", orbbecDevs.front());
            orbbecDevs.erase(orbbecDevs.begin());
        } else if (rightPreferred == "hikvision" && !hikDevs.empty()) {
            changed |= assignSlot("right", hikDevs.front());
            hikDevs.erase(hikDevs.begin());
        }
    }
    if (!slots_["left"].connected) {
        if (!orbbecDevs.empty()) {
            changed |= assignSlot("left", orbbecDevs.front());
            orbbecDevs.erase(orbbecDevs.begin());
        } else if (!hikDevs.empty()) {
            changed |= assignSlot("left", hikDevs.front());
            hikDevs.erase(hikDevs.begin());
        }
    }
    if (!slots_["right"].connected) {
        if (!hikDevs.empty()) {
            changed |= assignSlot("right", hikDevs.front());
            hikDevs.erase(hikDevs.begin());
        } else if (!orbbecDevs.empty()) {
            changed |= assignSlot("right", orbbecDevs.front());
            orbbecDevs.erase(orbbecDevs.begin());
        }
    }
    if (!slots_["head"].connected) {
        if (!orbbecDevs.empty()) {
            changed |= assignSlot("head", orbbecDevs.front());
            orbbecDevs.erase(orbbecDevs.begin());
        } else if (!hikDevs.empty()) {
            changed |= assignSlot("head", hikDevs.front());
            hikDevs.erase(hikDevs.begin());
        }
    }

    return changed;
}

bool DeviceManager::detectGrippers() {
    detectAndAssignGrippers();
    return !detectedGrippers_.empty();
}

bool DeviceManager::refreshDetectedGrippers() {
    std::lock_guard<std::mutex> lock(detectedInfoMutex_);
    uint64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (lastGripperRefreshUs_ > 0 && nowUs > lastGripperRefreshUs_
        && (nowUs - lastGripperRefreshUs_) < 3000000ULL) {
        return !detectedGrippers_.empty();
    }
    lastGripperRefreshUs_ = nowUs;
    // 轻量刷新只枚举串口 VID，不重新 open 串口，避免页面轮询抢占手动夹爪端口。
    detectedGrippers_.clear();

    auto portInfos = UmiGripper::enumerateComPortsWithVid();
    std::set<std::string> detectedManualPorts;
    for (auto& info : portInfos) {
        if (info.vid != 0x0E01 && info.vid != 0x0E02) continue;
        DetectedGripper dg;
        dg.type = "manual";
        dg.port = info.portName;
        dg.connected = true;
        detectedGrippers_.push_back(dg);
        detectedManualPorts.insert(info.portName);
    }

    for (auto& kv : gripperSlots_) {
        auto& slot = kv.second;
        if (!slot.connected || slot.gripperType != "manual" || !slot.gripper) continue;
        std::string port = slot.gripper->getPortName();
        bool portStillPresent = !port.empty() && detectedManualPorts.count(port) > 0;
        bool gripperAlive = slot.gripper->isConnected();
        if (portStillPresent && gripperAlive) continue;

        fprintf(stderr, "[DeviceManager] %s 手动夹爪已断开，等待重新连接 (%s)\n",
                kv.first.c_str(), port.c_str());
        slot.gripper->close();
        slot.connected = false;
    }

    // 电动夹爪目前通过 CAN 打开后保持在槽位中，轻量刷新不重新探测 CAN。
    // 如果槽位里已经有电动夹爪，必须确认最近收到过 CAN 反馈才报告给前端。
    // 否则 CAN 断开后软件连接标志仍可能保持 true，页面会误显示“有设备”。
    for (auto& kv : gripperSlots_) {
        const auto& slot = kv.second;
        auto* electric = dynamic_cast<ElectricGripper*>(slot.gripper.get());
        if (slot.connected && slot.gripperType == "electric" && electric && electric->isConnected()) {
            ElectricGripperFullState fullState;
            electric->getFullState(fullState);
            bool recentlyResponsive = fullState.hasData && fullState.timestamp > 0
                && nowUs >= fullState.timestamp
                && (nowUs - fullState.timestamp) <= 5000000ULL;
            if (!recentlyResponsive) continue;

            DetectedGripper dg;
            dg.type = "electric";
            dg.port = electric->getPortName();
            dg.connected = true;
            detectedGrippers_.push_back(dg);
        }
    }

    return !detectedGrippers_.empty();
}

bool DeviceManager::attachDetectedGrippersToEmptySlots(bool allowElectricScan) {
    // 手动夹爪：按端口重新枚举后，只补挂当前为空的左右槽位。
    auto portInfos = UmiGripper::enumerateComPortsWithVid();
    std::map<std::string, std::string> manualPortsBySide;
    for (const auto& info : portInfos) {
        if (info.vid == 0x0E01) manualPortsBySide["left"] = info.portName;
        else if (info.vid == 0x0E02) manualPortsBySide["right"] = info.portName;
    }

    bool changed = false;
    for (const auto& side : {std::string("left"), std::string("right")}) {
        auto& slot = gripperSlots_[side];
        if (slot.connected) continue;
        auto it = manualPortsBySide.find(side);
        if (it == manualPortsBySide.end()) continue;

        UmiGripper* gripper = dynamic_cast<UmiGripper*>(slot.gripper.get());
        if (!gripper) {
            auto fresh = std::make_unique<UmiGripper>();
            gripper = fresh.get();
            slot.gripper = std::move(fresh);
        }
        if (gripper->open(it->second)) {
            slot.gripperType = "manual";
            slot.connected = true;
            changed = true;
            fprintf(stderr, "[DeviceManager] 热插拔补挂 %s 夹爪槽: 手动夹爪 (%s)\n",
                    side.c_str(), it->second.c_str());
        }
    }

    bool hasConnectedElectric = false;
    for (const auto& kv : gripperSlots_) {
        if (kv.second.connected && kv.second.gripperType == "electric" && kv.second.gripper) {
            hasConnectedElectric = true;
            break;
        }
    }
    if (hasConnectedElectric) return changed;

    bool canAvailable = false;
    auto& canWrapper = ECanVciWrapper::sharedInstance();
    if (!allowElectricScan) return changed;
    char exePath[MAX_PATH] = {0};
    std::string exeDir = ".";
    if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) > 0) {
        exeDir = exePath;
        auto lastSlash = exeDir.find_last_of("\\/");
        if (lastSlash != std::string::npos) exeDir = exeDir.substr(0, lastSlash);
    }
    if (canWrapper.load(exeDir) || canWrapper.load("")) {
        if (canWrapper.openDevice(4, 0) && canWrapper.initCAN(0, 0x00, 0x14) && canWrapper.startCAN()) {
            canAvailable = true;
        } else {
            canWrapper.close();
        }
    }
    if (!canAvailable) return changed;

    std::string electricSlot;
    for (const auto& slotName : {std::string("left"), std::string("right"), std::string("extra")}) {
        auto it = gripperSlots_.find(slotName);
        if (it != gripperSlots_.end() && !it->second.connected) {
            electricSlot = slotName;
            break;
        }
    }
    if (electricSlot.empty()) return changed;

    auto gripper = std::make_unique<ElectricGripper>();
    if (gripper->openCAN(&canWrapper, 0x15)) {
        DetectedGripper dg;
        dg.type = "electric";
        dg.port = gripper->getPortName();
        dg.connected = true;
        detectedGrippers_.push_back(dg);
        gripperSlots_[electricSlot].gripperType = "electric";
        gripperSlots_[electricSlot].gripper = std::move(gripper);
        gripperSlots_[electricSlot].connected = true;
        fprintf(stderr, "[DeviceManager] 热插拔补挂 %s 夹爪槽: 电动夹爪 (CAN)\n", electricSlot.c_str());
        changed = true;
    }
    return changed;
}

void DeviceManager::detectOrbbecDevices() {
#ifndef NO_ORBBEC_CAMERA
    ob_error* err = nullptr;
    auto* ctx = ob_create_context(&err);
    if (err || !ctx) {
        fprintf(stderr, "[DeviceManager] Orbbec Context 创建失败: %s\n",
                err ? ob_error_get_message(err) : "unknown");
        if (err) ob_delete_error(err);
        return;
    }

    auto* devList = ob_query_device_list(ctx, &err);
    if (err) {
        fprintf(stderr, "[DeviceManager] Orbbec 设备查询失败: %s\n",
                err ? ob_error_get_message(err) : "unknown");
        if (err) ob_delete_error(err);
        ob_delete_context(ctx, &err);
        if (err) ob_delete_error(err);
        return;
    }

    int count = ob_device_list_get_device_count(devList, &err);
    if (err) { ob_delete_error(err); ob_delete_device_list(devList, &err); ob_delete_context(ctx, &err); return; }

    for (int i = 0; i < count; i++) {
        DetectedDevice dev;
        dev.type = "orbbec";
        dev.index = i;

        auto* device = ob_device_list_get_device(devList, i, &err);
        if (err) { ob_delete_error(err); continue; }

        auto* info = ob_device_get_device_info(device, &err);
        if (!err && info) {
            const char* name = ob_device_info_get_name(info, &err);
            if (!err && name) dev.name = name;
            if (err) { ob_delete_error(err); err = nullptr; }

            const char* sn = ob_device_info_get_serial_number(info, &err);
            if (!err && sn) dev.serialNumber = sn;
            if (err) { ob_delete_error(err); err = nullptr; }

            ob_delete_device_info(info, &err);
            if (err) { ob_delete_error(err); err = nullptr; }
        }
        if (err) ob_delete_error(err);

        // device 由 devList 管理，不单独释放

        detectedDevices_.push_back(dev);
        fprintf(stderr, "[DeviceManager] 检测到 Orbbec 设备: %s (SN: %s)\n",
                dev.name.c_str(), dev.serialNumber.c_str());
    }

    ob_delete_device_list(devList, &err);
    if (err) ob_delete_error(err);
    ob_delete_context(ctx, &err);
    if (err) ob_delete_error(err);
#else
    fprintf(stderr, "[DeviceManager] Orbbec SDK 未安装，跳过检测\n");
#endif
}

void DeviceManager::detectHikvisionDevices() {
#ifndef NO_HIK_CAMERA
    // 分开枚举 GigE 和 USB，同一个物理摄像头通过两种接口可能出现两次
    // 先 GigE 再 USB，按 serial + name 双重去重
    std::set<std::string> seenSerials;
    std::set<std::string> seenNames;

    auto enumByTransport = [&](unsigned int transportType, const char* transportName) {
        MV_CC_DEVICE_INFO_LIST devList;
        memset(&devList, 0, sizeof(devList));
        int ret = MV_CC_EnumDevices(transportType, &devList);
        if (ret != MV_OK || devList.nDeviceNum == 0) return;

        for (unsigned int i = 0; i < devList.nDeviceNum; i++) {
            auto* info = devList.pDeviceInfo[i];
            if (!info) continue;

            std::string serial, name;
            if (info->nTLayerType == MV_GIGE_DEVICE) {
                name = std::string((char*)info->SpecialInfo.stGigEInfo.chUserDefinedName);
                serial = std::string((char*)info->SpecialInfo.stGigEInfo.chSerialNumber);
            } else {
                name = std::string((char*)info->SpecialInfo.stUsb3VInfo.chUserDefinedName);
                serial = std::string((char*)info->SpecialInfo.stUsb3VInfo.chSerialNumber);
            }

            // 按 serial 或 name 去重
            if (seenSerials.count(serial) || (!name.empty() && seenNames.count(name))) {
                fprintf(stderr, "[DeviceManager] 跳过重复海康设备[%s]: %s (SN: %s)\n",
                        transportName, name.c_str(), serial.c_str());
                continue;
            }
            seenSerials.insert(serial);
            if (!name.empty()) seenNames.insert(name);

            DetectedDevice dev;
            dev.type = "hikvision";
            dev.index = i;  // 这是在本次枚举中的索引，open 时会按 serial 查找
            dev.name = name;
            dev.serialNumber = serial;

            detectedDevices_.push_back(dev);
            fprintf(stderr, "[DeviceManager] 检测到海康设备[%s]: %s (SN: %s, idx=%d)\n",
                    transportName, dev.name.c_str(), dev.serialNumber.c_str(), dev.index);
        }
    };

    enumByTransport(MV_GIGE_DEVICE, "GigE");
    enumByTransport(MV_USB_DEVICE, "USB");

    if (detectedDevices_.empty()) {
        // 没有按单传输找到，尝试组合枚举
        MV_CC_DEVICE_INFO_LIST devList;
        memset(&devList, 0, sizeof(devList));
        int ret = MV_CC_EnumDevices(MV_GIGE_DEVICE | MV_USB_DEVICE, &devList);
        if (ret == MV_OK) {
            for (unsigned int i = 0; i < devList.nDeviceNum; i++) {
                auto* info = devList.pDeviceInfo[i];
                if (!info) continue;
                std::string serial, name;
                if (info->nTLayerType == MV_GIGE_DEVICE) {
                    name = std::string((char*)info->SpecialInfo.stGigEInfo.chUserDefinedName);
                    serial = std::string((char*)info->SpecialInfo.stGigEInfo.chSerialNumber);
                } else {
                    name = std::string((char*)info->SpecialInfo.stUsb3VInfo.chUserDefinedName);
                    serial = std::string((char*)info->SpecialInfo.stUsb3VInfo.chSerialNumber);
                }
                DetectedDevice dev;
                dev.type = "hikvision";
                dev.index = i;
                dev.name = name;
                dev.serialNumber = serial;
                detectedDevices_.push_back(dev);
                fprintf(stderr, "[DeviceManager] 检测到海康设备[混合]: %s (SN: %s)\n",
                        name.c_str(), serial.c_str());
            }
        }
    }
#else
    fprintf(stderr, "[DeviceManager] 海康 SDK 未安装，跳过检测\n");
#endif
}

// ---- 槽位分配 ----

void DeviceManager::assignSlots() {
    // 读取配置中的优先设备类型
    std::string leftPreferred  = cfg_.devices.count("left")  ? cfg_.devices.at("left").preferredType  : "auto";
    std::string rightPreferred = cfg_.devices.count("right") ? cfg_.devices.at("right").preferredType : "auto";

    // 收集可用设备，按类型分组
    std::vector<DetectedDevice*> orbbecDevs, hikDevs;
    for (auto& d : detectedDevices_) {
        if (d.type == "orbbec") orbbecDevs.push_back(&d);
        else if (d.type == "hikvision") hikDevs.push_back(&d);
    }

    auto assignSlot = [&](const std::string& slotName, DetectedDevice* dev) {
        auto& slot = slots_[slotName];
        slot.deviceType = dev->type;
        slot.connected = false;

        if (dev->type == "orbbec") {
#ifndef NO_ORBBEC_CAMERA
            auto cam = std::make_unique<OrbbecCamera>();
            if (cam->open(dev->index, dev->serialNumber)) {
                // 检查是否和已有槽位的摄像头 serial 重复
                std::string sn = cam->getSerialNumber();
                bool dup = false;
                for (auto& kv : slots_) {
                    if (kv.first != slotName && kv.second.connected && kv.second.camera &&
                        kv.second.camera->getSerialNumber() == sn) {
                        fprintf(stderr, "[DeviceManager] %s 槽: 序列号 %s 与 %s 槽重复，跳过\n",
                                slotName.c_str(), sn.c_str(), kv.first.c_str());
                        dup = true;
                        break;
                    }
                }
                if (dup) return;
                slot.camera = std::move(cam);
                slot.connected = true;
                fprintf(stderr, "[DeviceManager] %s 槽: Orbbec %s (SN: %s)\n",
                        slotName.c_str(), dev->name.c_str(), sn.c_str());
            }
#endif
        } else if (dev->type == "hikvision") {
            auto cam = std::make_unique<HikCameraAdapter>(cfg_.camera);
            if (cam->open(dev->index, dev->serialNumber)) {
                std::string sn = cam->getSerialNumber();
                bool dup = false;
                for (auto& kv : slots_) {
                    if (kv.first != slotName && kv.second.connected && kv.second.camera &&
                        kv.second.camera->getSerialNumber() == sn) {
                        fprintf(stderr, "[DeviceManager] %s 槽: 序列号 %s 与 %s 槽重复，跳过\n",
                                slotName.c_str(), sn.c_str(), kv.first.c_str());
                        dup = true;
                        break;
                    }
                }
                if (dup) return;
                slot.camera = std::move(cam);
                slot.connected = true;
                fprintf(stderr, "[DeviceManager] %s 槽: Hikvision %s (SN: %s)\n",
                        slotName.c_str(), dev->name.c_str(), sn.c_str());
            }
        }
    };

    // 简单分配策略：先按配置偏好，再按检测顺序
    if (leftPreferred == "orbbec" && !orbbecDevs.empty()) {
        assignSlot("left", orbbecDevs[0]);
        orbbecDevs.erase(orbbecDevs.begin());
    } else if (leftPreferred == "hikvision" && !hikDevs.empty()) {
        assignSlot("left", hikDevs[0]);
        hikDevs.erase(hikDevs.begin());
    }

    if (rightPreferred == "orbbec" && !orbbecDevs.empty()) {
        assignSlot("right", orbbecDevs[0]);
        orbbecDevs.erase(orbbecDevs.begin());
    } else if (rightPreferred == "hikvision" && !hikDevs.empty()) {
        assignSlot("right", hikDevs[0]);
        hikDevs.erase(hikDevs.begin());
    }

    // auto 模式：填充剩余空槽
    if (!slots_["left"].connected) {
        if (!orbbecDevs.empty()) { assignSlot("left", orbbecDevs[0]); orbbecDevs.erase(orbbecDevs.begin()); }
        else if (!hikDevs.empty()) { assignSlot("left", hikDevs[0]); hikDevs.erase(hikDevs.begin()); }
    }
    if (!slots_["right"].connected) {
        if (!hikDevs.empty()) { assignSlot("right", hikDevs[0]); hikDevs.erase(hikDevs.begin()); }
        else if (!orbbecDevs.empty()) { assignSlot("right", orbbecDevs[0]); orbbecDevs.erase(orbbecDevs.begin()); }
    }
    // 第3个设备分配给 head
    if (!slots_["head"].connected) {
        if (!orbbecDevs.empty()) { assignSlot("head", orbbecDevs[0]); orbbecDevs.erase(orbbecDevs.begin()); }
        else if (!hikDevs.empty()) { assignSlot("head", hikDevs[0]); hikDevs.erase(hikDevs.begin()); }
    }

    for (auto& kv : slots_) {
        if (!kv.second.connected) {
            fprintf(stderr, "[DeviceManager] %s 槽: 无设备\n", kv.first.c_str());
        }
    }
}

// ---- 查询 ----

DeviceSlot* DeviceManager::getSlot(const std::string& position) {
    auto it = slots_.find(position);
    return it != slots_.end() ? &it->second : nullptr;
}

const DeviceSlot* DeviceManager::getSlot(const std::string& position) const {
    auto it = slots_.find(position);
    return it != slots_.end() ? &it->second : nullptr;
}

int DeviceManager::getOrbbecCount() const {
    int c = 0;
    for (auto& d : detectedDevices_) if (d.type == "orbbec") c++;
    return c;
}

int DeviceManager::getHikvisionCount() const {
    int c = 0;
    for (auto& d : detectedDevices_) if (d.type == "hikvision") c++;
    return c;
}

std::vector<std::string> DeviceManager::getSlotNames() const {
    std::vector<std::string> names;
    for (auto& kv : slots_) names.push_back(kv.first);
    return names;
}

// ---- 槽位操作 ----

bool DeviceManager::swapSlots(const std::string& pos1, const std::string& pos2) {
    auto it1 = slots_.find(pos1);
    auto it2 = slots_.find(pos2);
    if (it1 == slots_.end() || it2 == slots_.end()) return false;

    // 停止现有流
    if (it1->second.connected && it1->second.camera) it1->second.camera->stopStreaming();
    if (it2->second.connected && it2->second.camera) it2->second.camera->stopStreaming();

    std::swap(it1->second.deviceType, it2->second.deviceType);
    std::swap(it1->second.camera, it2->second.camera);
    std::swap(it1->second.connected, it2->second.connected);

    if (it1->second.connected && it1->second.camera) it1->second.camera->startStreaming();
    if (it2->second.connected && it2->second.camera) it2->second.camera->startStreaming();

    fprintf(stderr, "[DeviceManager] 已交换 %s/%s 槽位\n", pos1.c_str(), pos2.c_str());
    return true;
}

bool DeviceManager::assignCamera(const std::string& serial, const std::string& targetSlot) {
    // 策略：将指定 serial 的摄像头移到 targetSlot，
    // 然后把剩余的摄像头按顺序分配给 left、right

    if (slots_.find(targetSlot) == slots_.end()) return false;

    // 收集所有已分配到槽位的摄像头，先停止流
    struct SlotCam { std::string slot; std::string serial; std::string type;
                     std::unique_ptr<ICamera> cam; bool connected; };
    std::vector<SlotCam> cams;
    for (auto& kv : slots_) {
        if (kv.second.connected && kv.second.camera) {
            kv.second.camera->stopStreaming();
            cams.push_back({kv.first, kv.second.camera->getSerialNumber(),
                            kv.second.deviceType, std::move(kv.second.camera), kv.second.connected});
        }
        kv.second.connected = false;
        kv.second.camera.reset();
        kv.second.deviceType.clear();
    }

    // 找到目标 serial 的摄像头
    SlotCam* targetCam = nullptr;
    for (auto& c : cams) {
        if (c.serial == serial) { targetCam = &c; break; }
    }
    if (!targetCam) {
        fprintf(stderr, "[DeviceManager] 未找到序列号 %s 的摄像头\n", serial.c_str());
        // 恢复原来的分配
        for (auto& c : cams) {
            auto& slot = slots_[c.slot];
            slot.deviceType = c.type;
            slot.camera = std::move(c.cam);
            slot.connected = c.connected;
            if (slot.connected && slot.camera) slot.camera->startStreaming();
        }
        return false;
    }

    // 把目标摄像头分配到 targetSlot
    auto& tSlot = slots_[targetSlot];
    tSlot.deviceType = targetCam->type;
    tSlot.camera = std::move(targetCam->cam);
    tSlot.connected = targetCam->connected;
    targetCam->slot = targetSlot; // 标记已用

    // 剩余摄像头分配给 left、right
    std::vector<std::string> freeSlots = {"left", "right"};
    for (auto& c : cams) {
        if (c.slot == targetSlot) continue; // 已分配到 targetSlot
        // 找一个空的 slot
        for (auto& fs : freeSlots) {
            if (fs == targetSlot) continue;
            auto& slot = slots_[fs];
            if (!slot.connected) {
                slot.deviceType = c.type;
                slot.camera = std::move(c.cam);
                slot.connected = c.connected;
                break;
            }
        }
    }

    // 启动所有流
    for (auto& kv : slots_) {
        if (kv.second.connected && kv.second.camera) {
            kv.second.camera->startStreaming();
            fprintf(stderr, "[DeviceManager] %s 槽: %s (SN: %s)\n",
                    kv.first.c_str(), kv.second.deviceType.c_str(),
                    kv.second.camera->getSerialNumber().c_str());
        }
    }
    return true;
}

// ---- 夹爪 ----

GripperSlot* DeviceManager::getGripperSlot(const std::string& position) {
    auto it = gripperSlots_.find(position);
    return it != gripperSlots_.end() ? &it->second : nullptr;
}

const GripperSlot* DeviceManager::getGripperSlot(const std::string& position) const {
    auto it = gripperSlots_.find(position);
    return it != gripperSlots_.end() ? &it->second : nullptr;
}

std::vector<std::string> DeviceManager::getGripperSlotNames() const {
    std::vector<std::string> names;
    for (auto& kv : gripperSlots_) names.push_back(kv.first);
    return names;
}

void DeviceManager::detectAndAssignGrippers() {
    detectedGrippers_.clear();

    // 清除旧分配
    for (auto& kv : gripperSlots_) {
        kv.second.connected = false;
        kv.second.gripperType = "none";
        kv.second.gripper.reset();
    }

    fprintf(stderr, "[DeviceManager] 扫描夹爪设备（通过 USB VID 检测左右手）...\n");

    // 通过 SetupAPI 枚举带 VID 的串口
    auto portInfos = UmiGripper::enumerateComPortsWithVid();

    // 筛选手动夹爪（VID=0x0E01 左手, VID=0x0E02 右手, PID=0xA001）
    struct GripperCandidate {
        std::string port;
        std::string side;  // "left" or "right"
    };
    std::vector<GripperCandidate> candidates;

    for (auto& info : portInfos) {
        if (info.vid == 0x0E01 || info.vid == 0x0E02) {
            GripperCandidate gc;
            gc.port = info.portName;
            if (info.vid == 0x0E01) {
                gc.side = "left";
            } else {
                gc.side = "right";
            }
            candidates.push_back(gc);
            fprintf(stderr, "[DeviceManager] 检测到手动夹爪: %s (VID=0x%04X PID=0x%04X -> %s手)\n",
                info.portName.c_str(), info.vid, info.pid, gc.side.c_str());
        }
    }

    // 如果 SetupAPI 没找到，回退到逐个扫描串口并尝试打开
    if (candidates.empty()) {
        fprintf(stderr, "[DeviceManager] SetupAPI 未检测到夹爪 VID，回退到串口扫描...\n");
        auto ports = UmiGripper::scanSerialPorts();
        for (auto& port : ports) {
            auto gripper = std::make_unique<UmiGripper>();
            if (gripper->open(port)) {
                GripperCandidate gc;
                gc.port = port;
                gc.side = gripper->getHandSide();
                candidates.push_back(gc);
                gripper->close();
                fprintf(stderr, "[DeviceManager] 串口扫描检测到夹爪: %s (%s手)\n",
                    port.c_str(), gc.side.c_str());
            }
        }
    }

    // 尝试电动夹爪（通过 CAN 适配器检测）
    std::set<std::string> usedPorts;
    for (auto& c : candidates) usedPorts.insert(c.port);

    // 尝试加载 CAN SDK 并检测电动夹爪
    fprintf(stderr, "[DeviceManager] 尝试检测 CAN 电动夹爪...\n");
    auto& canWrapper = ECanVciWrapper::sharedInstance();

    // 搜索 DLL 路径：exe目录下
    std::string exeDir;
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    exeDir = exePath;
    auto lastSlash = exeDir.find_last_of("\\/");
    if (lastSlash != std::string::npos) exeDir = exeDir.substr(0, lastSlash);

    bool canAvailable = false;
    if (canWrapper.load(exeDir)) {
        // 打印结构体大小，帮助诊断对齐问题
        fprintf(stderr, "[CAN] 结构体大小: CAN_OBJ=%zu INIT_CONFIG=%zu BOARD_INFO=%zu\n",
                sizeof(ECAN_CAN_OBJ), sizeof(ECAN_INIT_CONFIG), sizeof(ECAN_BOARD_INFO));

        if (canWrapper.openDevice(4, 0)) { // USBCAN2, device 0
            // 读取板卡信息确认适配器可用
            ECAN_BOARD_INFO boardInfo = {};
            if (canWrapper.readBoardInfo(boardInfo)) {
                fprintf(stderr, "[CAN] 板卡: HW=%u FW=%u 串号=%.20s CAN通道=%u\n",
                        boardInfo.hw_Version, boardInfo.fw_Version,
                        boardInfo.str_Serial_Num, boardInfo.can_Num);
            } else {
                fprintf(stderr, "[CAN] 读取板卡信息失败\n");
            }

            if (canWrapper.initCAN(0, 0x00, 0x14)) { // Channel 0, 1Mbps
                if (canWrapper.startCAN()) {
                    canAvailable = true;
                    fprintf(stderr, "[DeviceManager] CAN 适配器已打开 (通道0, 1Mbps)\n");
                } else {
                    fprintf(stderr, "[DeviceManager] CAN StartCAN 失败\n");
                    canWrapper.close();
                }
            } else {
                fprintf(stderr, "[DeviceManager] CAN InitCAN 失败\n");
                canWrapper.close();
            }
        } else {
            fprintf(stderr, "[DeviceManager] 未检测到 CAN 适配器（可能未连接）\n");
        }
    } else {
        fprintf(stderr, "[DeviceManager] ECanVci64.dll 未找到，跳过 CAN 电动夹爪检测\n");
    }

    // 按 USB VID 固定分配左右手
    for (auto& c : candidates) {
        auto& slot = gripperSlots_[c.side];
        slot.gripperType = "manual";

        auto gripper = std::make_unique<UmiGripper>();
        if (gripper->open(c.port)) {
            DetectedGripper dg;
            dg.type = "manual";
            dg.port = c.port;
            dg.connected = true;
            detectedGrippers_.push_back(dg);

            slot.gripper = std::move(gripper);
            slot.connected = true;
            fprintf(stderr, "[DeviceManager] %s 夹爪槽: 手动夹爪 (%s, VID固定)\n",
                    c.side.c_str(), c.port.c_str());
        }
    }

    // 电动夹爪：优先沿用 left/right 空槽；两个手动夹爪都已连接时，放入 extra 额外槽。
    std::string electricSlot;
    for (const auto& slotName : {std::string("left"), std::string("right"), std::string("extra")}) {
        auto it = gripperSlots_.find(slotName);
        if (it != gripperSlots_.end() && !it->second.connected) {
            electricSlot = slotName;
            break;
        }
    }
    if (!electricSlot.empty() && canAvailable) {
        auto gripper = std::make_unique<ElectricGripper>();
        if (gripper->openCAN(&canWrapper, 0x15)) {
            DetectedGripper dg;
            dg.type = "electric";
            dg.port = gripper->getPortName();
            dg.connected = true;
            detectedGrippers_.push_back(dg);
            gripperSlots_[electricSlot].gripperType = "electric";
            gripperSlots_[electricSlot].gripper = std::move(gripper);
            gripperSlots_[electricSlot].connected = true;
            fprintf(stderr, "[DeviceManager] %s 夹爪槽: 电动夹爪 (CAN)\n", electricSlot.c_str());
        }
    }
}

std::string DeviceManager::toJson() const {
    std::lock_guard<std::mutex> lock(detectedInfoMutex_);
    std::set<std::string> detectedCameraSerials;
    for (const auto& d : detectedDevices_) {
        if (!d.serialNumber.empty()) detectedCameraSerials.insert(d.serialNumber);
    }

    std::set<std::string> detectedGripperPorts;
    for (const auto& g : detectedGrippers_) {
        if (g.connected && !g.port.empty()) detectedGripperPorts.insert(g.port);
    }
    uint64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::string json = "{\"devices\":[";
    for (size_t i = 0; i < detectedDevices_.size(); i++) {
        auto& d = detectedDevices_[i];
        if (i > 0) json += ",";
        json += "{\"type\":\"" + d.type + "\",\"name\":\"" + d.name +
                "\",\"serial\":\"" + d.serialNumber + "\"}";
    }
    json += "],\"slots\":{";
    bool first = true;
    for (auto& kv : slots_) {
        if (!first) json += ",";
        first = false;
        bool slotConnected = kv.second.connected && kv.second.camera;
        std::string slotSerial;
        if (slotConnected) {
            slotSerial = kv.second.camera->getSerialNumber();
            // Orbbec 热插拔断开时已经在 refreshDetectedCameras() 里释放旧对象，
            // 这里不再强依赖序列号二次校验，避免“已检测到设备但页面仍显示未连接”。
            if (kv.second.deviceType == "orbbec") {
                slotConnected = kv.second.camera->isOpened();
            } else {
                slotConnected = !slotSerial.empty() && detectedCameraSerials.count(slotSerial) > 0;
            }
        }
        json += "\"" + kv.first + "\":{\"type\":\"" + kv.second.deviceType +
                "\",\"connected\":" + (slotConnected ? "true" : "false");
        if (slotConnected && kv.second.camera) {
            json += ",\"name\":\"" + kv.second.camera->getDeviceName() +
                    "\",\"serial\":\"" + slotSerial + "\"";
            json += ",\"hasDepth\":" + std::string(kv.second.camera->hasDepthStream() ? "true" : "false");
            json += ",\"hasIMU\":false";
            json += ",\"hasIR\":" + std::string(kv.second.camera->hasIRStream() ? "true" : "false");
        }
        json += "}";
    }
    json += "},\"grippers\":[";
    for (size_t i = 0; i < detectedGrippers_.size(); i++) {
        if (i > 0) json += ",";
        auto& g = detectedGrippers_[i];
        json += "{\"type\":\"" + g.type + "\",\"port\":\"" + g.port +
                "\",\"connected\":" + (g.connected ? "true" : "false") + "}";
    }
    json += "],\"gripperSlots\":{";
    bool gfirst = true;
    for (auto& kv : gripperSlots_) {
        if (!gfirst) json += ",";
        gfirst = false;
        bool gripperConnected = kv.second.connected && kv.second.gripper;
        std::string gripperPort;
        if (gripperConnected) {
            gripperPort = kv.second.gripper->getPortName();
            if (kv.second.gripperType == "manual") {
                gripperConnected = !gripperPort.empty() && detectedGripperPorts.count(gripperPort) > 0;
            } else if (kv.second.gripperType == "electric") {
                auto* electric = dynamic_cast<ElectricGripper*>(kv.second.gripper.get());
                ElectricGripperFullState fullState;
                if (electric) electric->getFullState(fullState);
                gripperConnected = electric && electric->isConnected()
                    && fullState.hasData && fullState.timestamp > 0
                    && nowUs >= fullState.timestamp
                    && (nowUs - fullState.timestamp) <= 5000000ULL;
            }
        }
        json += "\"" + kv.first + "\":{\"type\":\"" + kv.second.gripperType +
                "\",\"connected\":" + (gripperConnected ? "true" : "false");
        if (gripperConnected && kv.second.gripper) {
            json += ",\"port\":\"" + gripperPort + "\"";
        }
        json += "}";
    }
    json += "}}";
    return json;
}
