// UmiGripper.cpp - UMI 手动夹爪 V2 驱动（Windows 版）
// 基于 Win32 串口 API（CreateFile/DCB/ReadFile/WriteFile）

#include "UmiGripper.hpp"

#include <windows.h>
#include <setupapi.h>

// 某些 MinGW 头文件未定义该 GUID，这里补充声明用于 SetupAPI 枚举串口设备。
#ifndef GUID_DEVINTERFACE_COMPORT
static const GUID GUID_DEVINTERFACE_COMPORT = \
    {0x86E0D1E0, 0x8089, 0x11D0, {0x9C, 0xE4, 0x08, 0x00, 0x3E, 0x30, 0x1F, 0x73}};
#endif
#include <cstring>
#include <chrono>
#include <cstdio>
#include <string>
#include <set>
#include <algorithm>

namespace {
constexpr int kDefaultBaudRate = 115200;
constexpr size_t kResponsePacketSize = 8;
constexpr uint8_t kPacketHead = 0x0A;
constexpr uint8_t kTailLeft  = 0x0A;
constexpr uint8_t kTailRight = 0x0C;

// LED 命令：共 7 字节，用于设置夹爪指示灯颜色和亮度。
// 格式：[0x0A 头] [R] [G] [B] [0x00] [brightness] [0x0B 尾]
constexpr size_t kLedCmdSize = 7;
constexpr uint8_t kLedTail = 0x0B;

// 数据流控制命令：共 10 字节，用于启动、停止或切换 MCU 数据上报。
// 格式：[0x00 地址] [0x02 频率] [datahead 0x03左/0x04右] [0x0A 响应头]
//      [action 0/1/2] [R] [G] [B] [brightness] [0x0B 尾]
constexpr size_t kStreamCmdSize = 10;
constexpr uint8_t kLedAddr = 0x00;
constexpr uint8_t kLedFreq = 0x02;
constexpr uint8_t kLedDataHeadLeft  = 0x03;
constexpr uint8_t kLedDataHeadRight = 0x04;
constexpr uint8_t kLedResp = 0x0A;
constexpr uint8_t kStreamStartAction = 0x01;
constexpr uint8_t kStreamStopAction  = 0x02;

constexpr int kMaxFailCount = 10;

constexpr uint16_t kVidLeft  = 0x0E01;
constexpr uint16_t kVidRight = 0x0E02;
constexpr uint16_t kPidGripper = 0xA001;

bool isValidTail(uint8_t t) { return t == kTailLeft || t == kTailRight; }

uint8_t dataHeadForSide(const std::string& side) {
    return (side == "right") ? kLedDataHeadRight : kLedDataHeadLeft;
}
} // namespace

// ---- 通过 SetupAPI 按 USB VID 检测左右手 ----

std::vector<ComPortVidInfo> UmiGripper::enumerateComPortsWithVid() {
    std::vector<ComPortVidInfo> result;

    HDEVINFO hDevInfo = SetupDiGetClassDevs(&GUID_DEVINTERFACE_COMPORT, NULL, NULL,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hDevInfo == INVALID_HANDLE_VALUE) return result;

    SP_DEVICE_INTERFACE_DATA ifcData = {};
    ifcData.cbSize = sizeof(ifcData);

    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(hDevInfo, NULL, &GUID_DEVINTERFACE_COMPORT, i, &ifcData); i++) {
        // 第一次调用只获取缓冲区大小，随后再读取属性内容。
        DWORD reqSize = 0;
        SetupDiGetDeviceInterfaceDetailA(hDevInfo, &ifcData, NULL, 0, &reqSize, NULL);

        SP_DEVINFO_DATA devData = {};
        devData.cbSize = sizeof(devData);

        std::vector<uint8_t> buf(std::max(reqSize, (DWORD)sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A)));
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_A*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);

        if (!SetupDiGetDeviceInterfaceDetailA(hDevInfo, &ifcData, detail, (DWORD)buf.size(), NULL, &devData))
            continue;

        // 读取硬件 ID，从中解析 VID/PID。
        CHAR hwId[512] = {};
        if (!SetupDiGetDeviceRegistryPropertyA(hDevInfo, &devData, SPDRP_HARDWAREID,
            NULL, (PBYTE)hwId, sizeof(hwId), NULL))
            continue;

        std::string hwIdStr(hwId);
        uint16_t vid = 0, pid = 0;
        auto vidPos = hwIdStr.find("VID_");
        auto pidPos = hwIdStr.find("PID_");
        if (vidPos != std::string::npos) {
            unsigned int v = 0;
            sscanf(hwIdStr.c_str() + vidPos + 4, "%x", &v);
            vid = (uint16_t)v;
        }
        if (pidPos != std::string::npos) {
            unsigned int p = 0;
            sscanf(hwIdStr.c_str() + pidPos + 4, "%x", &p);
            pid = (uint16_t)p;
        }

        // 读取友好名称，从括号中提取 COM 端口号。
        CHAR friendly[256] = {};
        if (!SetupDiGetDeviceRegistryPropertyA(hDevInfo, &devData, SPDRP_FRIENDLYNAME,
            NULL, (PBYTE)friendly, sizeof(friendly), NULL))
            continue;

        std::string friendlyStr(friendly);
        auto comPos = friendlyStr.find("(COM");
        if (comPos == std::string::npos) continue;

        auto endPos = friendlyStr.find(')', comPos);
        if (endPos == std::string::npos) continue;

        ComPortVidInfo info;
        info.portName = friendlyStr.substr(comPos + 1, endPos - comPos - 1);
        info.vid = vid;
        info.pid = pid;
        result.push_back(info);

        fprintf(stderr, "[UmiGripper] COM port: %s  VID=0x%04X  PID=0x%04X\n",
            info.portName.c_str(), info.vid, info.pid);
    }

    SetupDiDestroyDeviceInfoList(hDevInfo);
    return result;
}

uint16_t UmiGripper::queryVidForPort(const std::string& portName) {
    auto ports = enumerateComPortsWithVid();
    for (auto& p : ports) {
        if (p.portName == portName) return p.vid;
    }
    return 0;
}

// ---- 构造与析构 ----

UmiGripper::UmiGripper()
    : hSerial_(INVALID_HANDLE_VALUE), connected_(false), running_(false),
      lastLedR_(0), lastLedG_(0), lastLedB_(0) {
    memset(&state_, 0, sizeof(state_));
}

UmiGripper::~UmiGripper() { close(); }

// ---- 打开与关闭串口 ----

bool UmiGripper::open(const std::string& port, int baudRate) {
    close();

    std::string winPort = "\\\\.\\" + port;

    // 双打开策略：先打开再关闭一次用于复位 USB 串口驱动，然后重新打开正式通信。
    {
        HANDLE hInit = CreateFileA(winPort.c_str(), GENERIC_READ | GENERIC_WRITE,
                                   0, NULL, OPEN_EXISTING, 0, NULL);
        if (hInit != INVALID_HANDLE_VALUE) {
            PurgeComm(hInit, PURGE_RXCLEAR | PURGE_TXCLEAR | PURGE_RXABORT | PURGE_TXABORT);
            CloseHandle(hInit);
        }
        Sleep(500);
    }

    hSerial_ = CreateFileA(winPort.c_str(), GENERIC_READ | GENERIC_WRITE,
                           0, NULL, OPEN_EXISTING, 0, NULL);
    if (hSerial_ == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[UMI夹爪] 无法打开 %s (error=%lu)\n", port.c_str(), GetLastError());
        return false;
    }

    if (!configurePort(baudRate)) {
        fprintf(stderr, "[UMI夹爪] 配置串口失败 %s\n", port.c_str());
        CloseHandle(hSerial_);
        hSerial_ = INVALID_HANDLE_VALUE;
        return false;
    }

    portName_ = port;
    PurgeComm(hSerial_, PURGE_RXCLEAR | PURGE_TXCLEAR | PURGE_RXABORT | PURGE_TXABORT);
    ClearCommError(hSerial_, NULL, NULL);

    fprintf(stderr, "[UMI夹爪] 串口 %s 已打开, 波特率=%d\n", port.c_str(), baudRate);

    // 优先通过 USB VID 判断左右手，避免仅依赖数据帧尾字节造成误判。
    detectHandSide();

    // 等待 USB 串口驱动稳定，减少刚打开端口时的首包丢失。
    Sleep(500);

    // 第一步：发送停止命令，清除 MCU 可能残留的上报状态。
    {
        uint8_t stopCmd[kLedCmdSize] = {kPacketHead, 0x02, 0x00, 0x00, 0x00, 0x00, kLedTail};
        DWORD written = 0;
        WriteFile(hSerial_, stopCmd, kLedCmdSize, &written, NULL);
        FlushFileBuffers(hSerial_);
        fprintf(stderr, "[UMI夹爪] 停止命令已发送 (%lu bytes)\n", written);
    }
    Sleep(100);
    PurgeComm(hSerial_, PURGE_RXCLEAR | PURGE_TXCLEAR);

    // 第二步：发送启动上报命令；没有这一步 MCU 不会主动发送夹爪数据。
    {
        uint8_t startCmd[kLedCmdSize] = {kPacketHead, 0x01, 0x00, 0x00, 0x00, 0x00, kLedTail};
        DWORD written = 0;
        WriteFile(hSerial_, startCmd, kLedCmdSize, &written, NULL);
        FlushFileBuffers(hSerial_);
        fprintf(stderr, "[UMI夹爪] 启动数据流命令已发送 (%lu bytes)\n", written);
    }

    // 第三步：设置 LED，用颜色提示当前夹爪已经连接。
    {
        uint8_t ledCmd[kLedCmdSize] = {kPacketHead, 0x00, 10, 10, 10, 64, kLedTail};
        DWORD written = 0;
        WriteFile(hSerial_, ledCmd, kLedCmdSize, &written, NULL);
        FlushFileBuffers(hSerial_);
        fprintf(stderr, "[UMI夹爪] LED唤醒命令已发送 (%lu bytes)\n", written);
    }

    // 等待 MCU 开始稳定上报数据。
    fprintf(stderr, "[UMI夹爪] 等待MCU响应...\n");
    Sleep(500);

    COMMTIMEOUTS timeouts;
    timeouts.ReadIntervalTimeout = 100;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = 5000;
    SetCommTimeouts(hSerial_, &timeouts);

    // 按字节读取原始数据，便于定位协议头尾错位或串口噪声问题。
    uint8_t rawBuf[64];
    DWORD totalRead = 0;
    for (int attempt = 0; attempt < 64 && totalRead < sizeof(rawBuf); attempt++) {
        uint8_t oneByte;
        DWORD n = 0;
        if (!ReadFile(hSerial_, &oneByte, 1, &n, NULL) || n == 0) break;
        rawBuf[totalRead++] = oneByte;
    }

    if (totalRead == 0) {
        DWORD errors = 0;
        COMSTAT comStat;
        ClearCommError(hSerial_, &errors, &comStat);
        fprintf(stderr, "[UMI夹爪] 无数据 (drvErrors=0x%lX, inQueue=%lu, outQueue=%lu)\n",
                errors, comStat.cbInQue, comStat.cbOutQue);
        CloseHandle(hSerial_);
        hSerial_ = INVALID_HANDLE_VALUE;
        return false;
    }

    fprintf(stderr, "[UMI夹爪] 收到 %lu 字节:", totalRead);
    for (DWORD i = 0; i < totalRead; i++) fprintf(stderr, " %02X", rawBuf[i]);
    fprintf(stderr, "\n");

    // 恢复非阻塞超时，避免轮询线程被串口读取长时间卡住。
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = 50;
    SetCommTimeouts(hSerial_, &timeouts);

    connected_ = true;
    state_.connected = true;
    running_ = true;
    pollThread_ = std::thread(&UmiGripper::pollLoop, this);

    fprintf(stderr, "[UMI夹爪] 已连接 %s (%s手)\n", port.c_str(), handSide_.c_str());
    return true;
}

void UmiGripper::close() {
    running_ = false;
    connected_ = false;
    state_.connected = false;
    if (pollThread_.joinable()) pollThread_.join();
    if (hSerial_ != INVALID_HANDLE_VALUE) {
        setLed(0, 0, 0, 0);
        uint8_t dataHead = dataHeadForSide(handSide_);
        uint8_t stopCmd[kStreamCmdSize] = {kLedAddr, kLedFreq, dataHead, kLedResp,
                                         kStreamStopAction, 0, 0, 0, 0, kLedTail};
        sendCommand(stopCmd, kStreamCmdSize);
        CloseHandle(hSerial_);
        hSerial_ = INVALID_HANDLE_VALUE;
    }
}

bool UmiGripper::isConnected() const { return connected_; }

void UmiGripper::getState(GripperState& out) const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    out = state_;
}

void UmiGripper::setLed(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness) {
    uint8_t cmd[kLedCmdSize] = {kPacketHead, 0x00, r, g, b, brightness, kLedTail};
    fprintf(stderr, "[LED] %s手 发送: %02X %02X %02X %02X %02X %02X %02X\n",
            handSide_.c_str(), cmd[0], cmd[1], cmd[2], cmd[3], cmd[4], cmd[5], cmd[6]);
    sendCommand(cmd, kLedCmdSize);
}

// ---- 串口扫描 ----

std::vector<std::string> UmiGripper::scanSerialPorts() {
    std::vector<std::string> ports;
    for (int i = 1; i <= 256; i++) {
        std::string portName = "\\\\.\\COM" + std::to_string(i);
        HANDLE h = CreateFileA(portName.c_str(), GENERIC_READ | GENERIC_WRITE,
                               0, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            ports.push_back("COM" + std::to_string(i));
        }
    }
    return ports;
}

// ---- 左右手识别 ----

void UmiGripper::detectHandSide() {
    handSide_ = "left";  // default
    vidConfirmed_ = false;

    // 优先通过 USB VID 判断左右手。
    uint16_t vid = queryVidForPort(portName_);
    if (vid == kVidLeft) {
        handSide_ = "left";
        vidConfirmed_ = true;
        fprintf(stderr, "[UMI夹爪] VID=0x%04X -> 左手\n", vid);
        return;
    } else if (vid == kVidRight) {
        handSide_ = "right";
        vidConfirmed_ = true;
        fprintf(stderr, "[UMI夹爪] VID=0x%04X -> 右手\n", vid);
        return;
    }

    // 兜底：如果端口名为空，首次收到数据帧后再根据帧尾字节判断左右手。
    fprintf(stderr, "[UMI夹爪] VID=0x%04X 未知，等待从数据帧尾针检测左右手\n", vid);
}

// ---- 串口参数配置 ----

bool UmiGripper::configurePort(int baudRate) {
    // 从零构造 DCB，避免复用系统默认串口配置带来的隐藏状态。
    DCB dcb;
    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);

    char dcbStr[64];
    snprintf(dcbStr, sizeof(dcbStr), "baud=%d parity=N data=8 stop=1", baudRate);
    if (!BuildCommDCBA(dcbStr, &dcb)) return false;

    // 显式关闭所有流控，保证 MCU 协议按裸串口方式收发。
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fAbortOnError = FALSE;

    if (!SetCommState(hSerial_, &dcb)) return false;

    COMMTIMEOUTS timeouts;
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = 50;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 100;
    SetCommTimeouts(hSerial_, &timeouts);

    SetupComm(hSerial_, 4096, 4096);
    return true;
}

// ---- 串口轮询循环 ----

void UmiGripper::pollLoop() {
    int failCount = 0;
    while (running_) {
        if (hSerial_ == INVALID_HANDLE_VALUE) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        uint8_t buffer[kResponsePacketSize];
        DWORD bytesRead = 0;
        if (!ReadFile(hSerial_, buffer, kResponsePacketSize, &bytesRead, NULL)
            || bytesRead != kResponsePacketSize) {
            failCount++;
            if (failCount >= kMaxFailCount) {
                fprintf(stderr, "[UMI夹爪] 连续通信失败，标记为断开\n");
                connected_ = false;
                state_.connected = false;
                return;
            }
            continue;
        }

        if (buffer[0] != kPacketHead || !isValidTail(buffer[kResponsePacketSize - 1])) {
            if (!syncFrame(buffer)) {
                failCount++;
                continue;
            }
        }

        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            memcpy(&state_.position, &buffer[1], 4);
            state_.button1 = buffer[5];
            state_.button2 = buffer[6];
            state_.timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            state_.hasData = true;

            // 只有 VID 未确认左右手时，才根据数据帧尾字节补充判断。
            if (!vidConfirmed_) {
                uint8_t tail = buffer[kResponsePacketSize - 1];
                if (tail == kTailRight && handSide_ != "right") {
                    handSide_ = "right";
                    vidConfirmed_ = true;
                    fprintf(stderr, "[UMI夹爪 %s] 尾针=0x0C -> 右手\n", portName_.c_str());
                } else if (tail == kTailLeft && handSide_ != "left") {
                    handSide_ = "left";
                    vidConfirmed_ = true;
                    fprintf(stderr, "[UMI夹爪 %s] 尾针=0x0A -> 左手\n", portName_.c_str());
                }
            }

        }
        failCount = 0;
    }
}

bool UmiGripper::syncFrame(uint8_t* buffer) {
    constexpr int kBufferCapacity = 4 * kResponsePacketSize;
    uint8_t buf[kBufferCapacity];
    memcpy(buf, buffer, kResponsePacketSize);

    for (int i = 0; i < kBufferCapacity - (int)kResponsePacketSize; ++i) {
        DWORD bytesRead = 0;
        if (!ReadFile(hSerial_, &buf[kResponsePacketSize + i], 1, &bytesRead, NULL) || bytesRead != 1)
            return false;

        memcpy(buffer, &buf[i + 1], kResponsePacketSize);
        if (buffer[0] == kPacketHead && isValidTail(buffer[kResponsePacketSize - 1])) {
            return true;
        }
    }
    return false;
}

// ---- 命令发送 ----

bool UmiGripper::sendCommand(const uint8_t* data, size_t len) {
    if (hSerial_ == INVALID_HANDLE_VALUE || data == nullptr) return false;
    std::lock_guard<std::mutex> lock(serialMutex_);
    PurgeComm(hSerial_, PURGE_RXCLEAR);
    DWORD written = 0;
    if (!WriteFile(hSerial_, data, (DWORD)len, &written, NULL)) return false;
    FlushFileBuffers(hSerial_);
    return written == len;
}
