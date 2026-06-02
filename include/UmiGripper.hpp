// UmiGripper.hpp - UMI 手动夹爪 V2 驱动（Windows 版）
// 通过 Win32 串口 API 与夹爪 MCU 通信
// 协议：MCU→PC [0x0A, pos_float[4], btn1, btn2, 0x0A=左/0x0C=右]
//       PC→MCU LED: [0x0A, 0x00, R, G, B, brightness, 0x0B]
//       PC→MCU Stream: [0x00, 0x02, 0x03L/0x04R, 0x0A, action, 0, 0, 0, 0, 0x0B]

#pragma once

#include <winsock2.h>
#include <windows.h>

#include "IGripper.hpp"
#include <mutex>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <cstdint>

struct ComPortVidInfo {
    std::string portName;  // "COM3"
    uint16_t vid = 0;
    uint16_t pid = 0;
};

class UmiGripper : public IGripper {
public:
    UmiGripper();
    ~UmiGripper() override;

    bool open(const std::string& port, int baudRate = 115200) override;
    void close() override;
    bool isConnected() const override;
    void getState(GripperState& out) const override;
    std::string getGripperType() const override { return "manual"; }
    std::string getPortName() const override { return portName_; }
    void setLed(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness = 100) override;
    static std::vector<std::string> scanSerialPorts();

    // USB VID detection
    static std::vector<ComPortVidInfo> enumerateComPortsWithVid();
    static uint16_t queryVidForPort(const std::string& portName);
    std::string getHandSide() const { return handSide_; }

private:
    void pollLoop();
    bool syncFrame(uint8_t* buffer);
    bool sendCommand(const uint8_t* data, size_t len);
    bool configurePort(int baudRate);
    void detectHandSide();

    HANDLE hSerial_ = INVALID_HANDLE_VALUE;
    std::atomic<bool> connected_{false};
    std::atomic<bool> running_{false};
    std::thread pollThread_;
    mutable std::mutex stateMutex_;
    std::mutex serialMutex_;
    GripperState state_;
    std::string portName_;
    std::string handSide_;  // "left" or "right", determined by USB VID (0x0E01=left, 0x0E02=right)
    bool vidConfirmed_ = false;  // VID 已确认左右手，不再用尾针覆盖
    uint8_t lastLedR_ = 0, lastLedG_ = 0, lastLedB_ = 0, lastLedBrightness_ = 0;
};
