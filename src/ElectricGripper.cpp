// ElectricGripper.cpp - 电动夹爪驱动（CAN总线版本）
// 广成科技 GCAN USBCAN + CANMIT 协议电机控制

#include "ElectricGripper.hpp"
#include <cstdio>
#include <cstring>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <vector>

namespace {
// MIT 参数范围
constexpr float KP_RANGE       = 1200.0f;   // mNm/rad
constexpr float KD_RANGE       = 500.0f;    // mNm/(rad/s)
constexpr float POSITION_RANGE = 12.5f;     // rad
constexpr float VELOCITY_RANGE = 12.56f;    // rad/s
constexpr float TORQUE_RANGE   = 450.0f;    // mNm
constexpr float PI = 3.14159265358979f;

// 夹爪行程参数（实测标定）
constexpr float CLAW_SAFE_TRAVEL_DEG = 4500.0f;  // Calibrated safe travel: 4500 deg
constexpr float CLAW_MAX_TRAVEL_DEG  = 5796.0f;  // 极限行程 16.1圈
constexpr float CLAW_SAFE_TRAVEL_RAD = 101.164f;  // 安全行程(弧度)

// CAN 配置帧 ID：协议约定 0x7FF 用于广播配置类命令。
constexpr uint32_t CAN_CONFIG_ID = 0x7FF;

uint64_t nowMicros() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool parseHexByte(const std::string& text, uint8_t& out) {
    char* end = nullptr;
    long value = std::strtol(text.c_str(), &end, 16);
    if (end == text.c_str() || *end != '\0' || value < 0 || value > 255) return false;
    out = static_cast<uint8_t>(value);
    return true;
}

bool parseHexId(const std::string& text, uint32_t& out) {
    char* end = nullptr;
    unsigned long value = std::strtoul(text.c_str(), &end, 16);
    if (end == text.c_str() || *end != '\0' || value > 0x7FF) return false;
    out = static_cast<uint32_t>(value);
    return true;
}
}

// ---- 构造/析构 ----

ElectricGripper::ElectricGripper() {
    memset(&state_, 0, sizeof(state_));
    memset(&fullState_, 0, sizeof(fullState_));
}

ElectricGripper::~ElectricGripper() { close(); }

// ---- IGripper 统一接口实现 ----

bool ElectricGripper::open(const std::string& port, int baudRate) {
    (void)baudRate;
    portName_ = port;
    // 为兼容 IGripper 通用接口，这里尝试自动检测 CAN 适配器。
    auto& can = ECanVciWrapper::sharedInstance();
    std::string dllDir = port;
    // 如果传入的 port 看起来像目录，则把它作为 CAN DLL 搜索路径。
    if (!can.load(dllDir)) {
        if (!can.load("")) {
            fprintf(stderr, "[电动夹爪] 无法加载 CAN SDK\n");
            return false;
        }
    }
    if (!can.openDevice(4, 0)) {
        fprintf(stderr, "[电动夹爪] 无法打开 CAN 设备\n");
        return false;
    }
    if (!can.initCAN(0, 0x00, 0x14)) { // Channel 0, 1Mbps
        fprintf(stderr, "[电动夹爪] 无法初始化 CAN 通道\n");
        can.close();
        return false;
    }
    if (!can.startCAN()) {
        fprintf(stderr, "[电动夹爪] 无法启动 CAN\n");
        can.close();
        return false;
    }
    // 使用固定的电机 ID 0x15。只有收到电机侧真实回包时才认为打开成功。
    if (!openCAN(&can, 0x15)) {
        can.close();
        return false;
    }
    return true;
}

bool ElectricGripper::openCAN(ECanVciWrapper* can, uint32_t motorId) {
    close();
    usingSerialBridge_ = false;
    can_ = can;
    motorId_ = motorId;
    lastMotorResponseUs_.store(0);
    connected_ = true;
    state_.connected = true;
    fullState_.connected = true;
    running_ = true;
    pollThread_ = std::thread(&ElectricGripper::pollLoop, this);
    portName_ = "CAN:" + std::to_string(motorId);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (!verifyMotorConnection(3000)) {
        fprintf(stderr, "[电动夹爪] CAN 适配器已打开，但没有收到电机 query_id 回包，请检查电机供电、CANH/CANL、共地、终端电阻和电机ID\n");
        close();
        return false;
    }

    fprintf(stderr, "[电动夹爪] 已连接, motorId=0x%03X\n", motorId_);
    return true;
}

bool ElectricGripper::openSerialBridge(const std::string& port, int baudRate, uint32_t motorId) {
    close();

    std::string winPort = "\\\\.\\" + port;
    serialBridge_ = CreateFileA(winPort.c_str(), GENERIC_READ | GENERIC_WRITE,
                                0, NULL, OPEN_EXISTING, 0, NULL);
    if (serialBridge_ == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[电动夹爪] 无法打开 ESP32-CAN 串口 %s (error=%lu)\n",
                port.c_str(), GetLastError());
        serialBridge_ = nullptr;
        return false;
    }

    if (!configureSerialBridge(baudRate)) {
        fprintf(stderr, "[电动夹爪] ESP32-CAN 串口配置失败: %s\n", port.c_str());
        CloseHandle(serialBridge_);
        serialBridge_ = nullptr;
        return false;
    }

    usingSerialBridge_ = true;
    motorId_ = motorId;
    lastMotorResponseUs_.store(0);
    portName_ = "ESP32-CAN:" + port;

    PurgeComm(serialBridge_, PURGE_RXCLEAR | PURGE_TXCLEAR | PURGE_RXABORT | PURGE_TXABORT);
    ClearCommError(serialBridge_, NULL, NULL);

    // ESP32-S3 打开串口后可能自动复位，留一点时间让固件打印启动信息并进入命令循环。
    Sleep(1200);
    writeSerialBridgeLine("diag off");
    // 平台需要读取 [RX] 日志来判断电机是否真的回包，因此接入平台时必须打开 rxlog。
    // 固件默认关闭 rxlog 是为了手动调试不刷屏，平台端会按行解析，不影响使用。
    writeSerialBridgeLine("rxlog on");
    {
        char idCmd[32];
        snprintf(idCmd, sizeof(idCmd), "id %X", (unsigned int)motorId_);
        writeSerialBridgeLine(idCmd);
    }
    writeSerialBridgeLine("status");

    if (!waitForSerialBridgeReady(2500)) {
        fprintf(stderr, "[电动夹爪] %s 没有识别为 can_transceiver 串口桥接器\n", port.c_str());
        CloseHandle(serialBridge_);
        serialBridge_ = nullptr;
        usingSerialBridge_ = false;
        return false;
    }

    if (!waitForSerialBridgeMotorResponse(3500)) {
        fprintf(stderr, "[电动夹爪] ESP32-CAN 串口桥接器已识别，但没有收到电机 query_id 回包: %s\n", port.c_str());
        fprintf(stderr, "[电动夹爪] 请检查夹爪供电、CANH/CANL、GND共地、120欧终端电阻，以及 ESP32 到 CAN 模块的 TX/RX 接线\n");
        CloseHandle(serialBridge_);
        serialBridge_ = nullptr;
        usingSerialBridge_ = false;
        return false;
    }

    connected_ = true;
    state_.connected = true;
    fullState_.connected = true;

    running_ = true;
    pollThread_ = std::thread(&ElectricGripper::pollLoop, this);
    fprintf(stderr, "[电动夹爪] 已连接 ESP32-CAN 串口桥接器: %s motorId=0x%03X\n",
            port.c_str(), motorId_);
    return true;
}

void ElectricGripper::close() {
    running_ = false;
    connected_ = false;
    state_.connected = false;
    fullState_.connected = false;
    if (pollThread_.joinable()) pollThread_.join();
    can_ = nullptr;
    if (hasSerialBridgeHandle()) {
        CloseHandle(serialBridge_);
        serialBridge_ = nullptr;
    }
    usingSerialBridge_ = false;
    serialBridgeRxLine_.clear();
    lastMotorResponseUs_.store(0);
}

bool ElectricGripper::isConnected() const { return connected_; }

void ElectricGripper::getState(GripperState& out) const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    out = state_;
}

float ElectricGripper::getPosition() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return state_.position;
}

void ElectricGripper::setPosition(float pos) {
    if (pos < 0.0f) pos = 0.0f;
    if (pos > 1.0f) pos = 1.0f;
    // 将通用接口的 0~1 归一化位置映射到实测 0~4500 度行程。
    float deg = pos * CLAW_SAFE_TRAVEL_DEG;
    sendPositionControl(deg, defaultSpeed_, defaultCurrent_);
}

void ElectricGripper::getFullState(ElectricGripperFullState& out) const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    out = fullState_;
}

bool ElectricGripper::hasRecentMotorResponse(uint64_t nowUs, uint64_t maxAgeUs) const {
    uint64_t last = lastMotorResponseUs_.load();
    return last > 0 && nowUs >= last && (nowUs - last) <= maxAgeUs;
}

bool ElectricGripper::verifyMotorConnection(int timeoutMs) {
    if (!connected_ || usingSerialBridge_) return false;

    uint64_t startUs = nowMicros();
    lastMotorResponseUs_.store(0);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    auto nextQuery = std::chrono::steady_clock::now();
    int queryRound = 0;

    while (std::chrono::steady_clock::now() < deadline) {
        auto now = std::chrono::steady_clock::now();
        if (now >= nextQuery) {
            queryMotorId();
            // 如果某些电机固件不响应广播查 ID，也用目标 ID 查询一次位置；
            // 只要收到任意电机反馈帧，就认为电机侧 CAN 总线已经连通。
            if ((queryRound % 2) == 1) queryPosition();
            queryRound++;
            nextQuery = now + std::chrono::milliseconds(350);
        }

        uint64_t last = lastMotorResponseUs_.load();
        if (last >= startUs) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    return false;
}

// ---- CAN 发送 ----

bool ElectricGripper::sendCANFrame(uint32_t id, const uint8_t* data, uint8_t len) {
    if (usingSerialBridge_) {
        if (!hasSerialBridgeHandle() || !connected_) {
            fprintf(stderr, "[电动夹爪] ESP32-CAN 发送失败: serial=%p connected=%d\n",
                    (void*)serialBridge_, connected_.load());
            return false;
        }
        if (len > 8) len = 8;

        char line[128];
        int offset = snprintf(line, sizeof(line), "raw %03X", (unsigned int)(id & 0x7FF));
        for (uint8_t i = 0; i < len && offset > 0 && offset < (int)sizeof(line); ++i) {
            offset += snprintf(line + offset, sizeof(line) - offset, " %02X", data[i]);
        }
        bool ok = writeSerialBridgeLine(line);
        if (ok) {
            markSerialBridgeAlive();
        } else {
            fprintf(stderr, "[电动夹爪] ESP32-CAN raw 发送失败: %s\n", line);
        }
        return ok;
    }

    if (!can_ || !connected_) {
        fprintf(stderr, "[电动夹爪] sendCANFrame 失败: can_=%p connected=%d\n",
                (void*)can_, connected_.load());
        return false;
    }
    ECAN_CAN_OBJ obj = {};
    obj.ID = id;
    obj.SendType = 1; // 1=单次发送(不重试), 避免无应答时阻塞
    obj.RemoteFlag = 0;
    obj.ExternFlag = 0;
    obj.DataLen = len;
    memcpy(obj.Data, data, len);
    bool ok = can_->transmit(obj);
    if (!ok) {
        fprintf(stderr, "[电动夹爪] Transmit 失败: ID=0x%03X len=%d data=[%02X %02X %02X %02X %02X %02X %02X %02X]\n",
                id, (int)len,
                len > 0 ? data[0] : 0, len > 1 ? data[1] : 0, len > 2 ? data[2] : 0, len > 3 ? data[3] : 0,
                len > 4 ? data[4] : 0, len > 5 ? data[5] : 0, len > 6 ? data[6] : 0, len > 7 ? data[7] : 0);
    }
    return ok;
}

bool ElectricGripper::sendConfigFrame(const uint8_t* data, uint8_t len) {
    return sendCANFrame(CAN_CONFIG_ID, data, len);
}

// ---- 电机控制字 ----

void ElectricGripper::enableMotor() {
    // 控制字：Enable=1，发送到电机 ID；data[0] 高 3 位为模式，低 5 位包含控制字。
    // 使用参数配置模式的返回类型来触发使能
    uint8_t data[8] = {};
    // 模式 = 当前模式(位置0x01), 控制字通过发送控制指令隐含使能
    // 直接发送一个0速度的位置保持指令来使能
    float deg = 0.0f;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        deg = fullState_.positionDeg;
    }
    sendPositionControl(deg, defaultSpeed_, defaultCurrent_);
}

void ElectricGripper::disableMotor() {
    // 发送失能：通过发送电流控制模式(reserve_state=2, 变阻尼制动)来实现
    uint8_t data[3] = {};
    data[0] = (0x03 << 5) | (0x02 << 2) | 0x00; // 模式=电流/刹车, reserve=2(变阻尼), return=0
    data[1] = 0;
    data[2] = 0;
    sendCANFrame(motorId_, data, 3);
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        fullState_.motorEnabled = false;
    }
}

void ElectricGripper::clearError() {
    // 通过发送空位置控制命令(return_type=0)间接清错
    // 直接发送一个带清错标志的帧
    uint8_t data[8] = {};
    data[0] = 0x01 << 5; // 位置模式, return_type=0(不返回)
    // 下发 float32 类型的 position=0、speed=0、current=0，作为清错的空位置命令。
    sendCANFrame(motorId_, data, 8);
}

void ElectricGripper::haltMotor() {
    // 发送停止(Halt=4): 使用刹车模式中的能耗制动
    uint8_t data[3] = {};
    data[0] = (0x03 << 5) | (0x03 << 2) | 0x00; // 模式=刹车, reserve=3(能耗制动)
    sendCANFrame(motorId_, data, 3);
}

// ---- 位置控制 (模式 0x01) ----

bool ElectricGripper::sendPositionControl(float positionDeg, float speedRpm, float currentLimit) {
    uint8_t data[8] = {};
    // 协议：位置单位为度°(float32)，直接使用
    float pos = positionDeg;
    if (pos > CLAW_SAFE_TRAVEL_DEG) pos = CLAW_SAFE_TRAVEL_DEG;
    if (pos < 0.0f) pos = 0.0f;
    uint32_t posBits;
    memcpy(&posBits, &pos, 4);

    // 速度编码：uint15，单位为 rpm*10。
    if (speedRpm < 0) speedRpm = 0;
    uint16_t speedCode = (uint16_t)(speedRpm * 10.0f);
    if (speedCode > 0x7FFF) speedCode = 0x7FFF;

    // 电流编码：uint12，单位为 A*10。位置控制只做非负保护，具体电流由用户按实验需要输入。
    if (currentLimit < 0) currentLimit = 0;
    uint16_t currentCode = (uint16_t)(currentLimit * 10.0f);
    if (currentCode > 0xFFF) currentCode = 0xFFF;

    // 反馈类型设为 1：返回位置、速度、电流和温度。
    uint8_t returnType = 1;

    // Byte0: [7:5]=mode(0x01), [4:0]=posBits[31:27]
    data[0] = (0x01 << 5) | ((posBits >> 27) & 0x1F);
    // Byte1: posBits[26:19]
    data[1] = (posBits >> 19) & 0xFF;
    // Byte2: posBits[18:11]
    data[2] = (posBits >> 11) & 0xFF;
    // Byte3: posBits[10:3]
    data[3] = (posBits >> 3) & 0xFF;
    // Byte4: [7:5]=posBits[2:0], [4:0]=speedCode[14:10]
    data[4] = ((posBits & 0x07) << 5) | ((speedCode >> 10) & 0x1F);
    // Byte5: speedCode[9:2]
    data[5] = (speedCode >> 2) & 0xFF;
    // Byte6: [7:6]=speedCode[1:0], [5:0]=currentCode[11:6]
    data[6] = ((speedCode & 0x03) << 6) | ((currentCode >> 6) & 0x3F);
    // Byte7: [7:2]=currentCode[5:0], [1:0]=returnType
    data[7] = ((currentCode & 0x3F) << 2) | (returnType & 0x03);

    return sendCANFrame(motorId_, data, 8);
}

// ---- 速度控制 (模式 0x02) ----

bool ElectricGripper::sendSpeedControl(float speedRpm, float currentLimit) {
    uint8_t data[7] = {};

    uint8_t reserveState = 0;
    uint8_t returnType = 1;

    float speed = speedRpm;
    uint32_t speedBits;
    memcpy(&speedBits, &speed, 4);

    if (currentLimit < 0) currentLimit = 0;
    uint16_t currentCode = (uint16_t)(currentLimit * 10.0f);

    data[0] = (0x02 << 5) | (reserveState << 2) | returnType;
    data[1] = (speedBits >> 24) & 0xFF;
    data[2] = (speedBits >> 16) & 0xFF;
    data[3] = (speedBits >> 8) & 0xFF;
    data[4] = speedBits & 0xFF;
    data[5] = (currentCode >> 8) & 0xFF;
    data[6] = currentCode & 0xFF;

    return sendCANFrame(motorId_, data, 7);
}

// ---- 极限位置测试 ----

// 慢速推向极限（正方向或负方向），用于标定行程
bool ElectricGripper::findLimit(float speedRpm, float currentLimit) {
    return sendSpeedControl(speedRpm, currentLimit);
}

// 停止电机（速度=0）
bool ElectricGripper::stopSpeed() {
    return sendSpeedControl(0.0f, DEFAULT_CURRENT_LIMIT);
}

// ---- MIT 力位混控 (模式 0x00) ----

bool ElectricGripper::sendMITControl(float kp, float kd, float positionRad, float speedRadS, float torque) {
    uint8_t data[8] = {};

    // kp: 12-bit (0~4095 → 0~KP_RANGE)
    uint16_t kpCode = (uint16_t)(kp / KP_RANGE * 4095.0f);
    if (kpCode > 4095) kpCode = 4095;

    // kd: 9-bit (0~511 → 0~KD_RANGE)
    uint16_t kdCode = (uint16_t)(kd / KD_RANGE * 511.0f);
    if (kdCode > 511) kdCode = 511;

    // position: 16-bit (-POSITION_RANGE ~ +POSITION_RANGE)
    float posNorm = (positionRad + POSITION_RANGE) / (POSITION_RANGE * 2.0f);
    uint16_t posCode = (uint16_t)(posNorm * 65535.0f);

    // speed: 12-bit (-VELOCITY_RANGE ~ +VELOCITY_RANGE)
    float spdNorm = (speedRadS + VELOCITY_RANGE) / (VELOCITY_RANGE * 2.0f);
    uint16_t spdCode = (uint16_t)(spdNorm * 4095.0f);
    if (spdCode > 4095) spdCode = 4095;

    // torque: 12-bit (-TORQUE_RANGE ~ +TORQUE_RANGE)
    float trqNorm = (torque + TORQUE_RANGE) / (TORQUE_RANGE * 2.0f);
    uint16_t trqCode = (uint16_t)(trqNorm * 4095.0f);
    if (trqCode > 4095) trqCode = 4095;

    data[0] = (0x00 << 5) | ((kpCode >> 7) & 0x1F);
    data[1] = ((kpCode & 0x7F) << 1) | ((kdCode >> 8) & 0x01);
    data[2] = kdCode & 0xFF;
    data[3] = (posCode >> 8) & 0xFF;
    data[4] = posCode & 0xFF;
    data[5] = (spdCode >> 4) & 0xFF;
    data[6] = ((spdCode & 0x0F) << 4) | ((trqCode >> 8) & 0x0F);
    data[7] = trqCode & 0xFF;

    return sendCANFrame(motorId_, data, 8);
}

// ---- 电流控制 (模式 0x03) ----

bool ElectricGripper::sendCurrentControl(float currentA) {
    uint8_t data[3] = {};
    if (currentA > MAX_CURRENT_CONTROL_A) currentA = MAX_CURRENT_CONTROL_A;
    if (currentA < -MAX_CURRENT_CONTROL_A) currentA = -MAX_CURRENT_CONTROL_A;
    int16_t currentCode = (int16_t)(currentA * 100.0f);
    uint8_t reserveState = 0; // 0=current control
    uint8_t returnType = 1;

    data[0] = (0x03 << 5) | (reserveState << 2) | returnType;
    data[1] = (currentCode >> 8) & 0xFF;
    data[2] = currentCode & 0xFF;

    return sendCANFrame(motorId_, data, 3);
}

// ---- 参数查询 (模式 0x07) ----

bool ElectricGripper::queryPosition() {
    uint8_t data[2] = {};
    data[0] = 0x07 << 5;
    data[1] = 0x01; // QUERY_POSITION = 1
    return sendCANFrame(motorId_, data, 2);
}

bool ElectricGripper::querySpeed() {
    uint8_t data[2] = {};
    data[0] = 0x07 << 5;
    data[1] = 0x02; // QUERY_SPEED = 2
    return sendCANFrame(motorId_, data, 2);
}

bool ElectricGripper::queryCurrent() {
    uint8_t data[2] = {};
    data[0] = 0x07 << 5;
    data[1] = 0x03; // QUERY_CURRENT = 3
    return sendCANFrame(motorId_, data, 2);
}

// ---- 参数配置 (模式 0x06) ----

bool ElectricGripper::setAcceleration(float accelRadS2) {
    uint8_t data[4] = {};
    uint8_t returnType = 1;
    data[0] = (0x06 << 5) | (0x00 << 2) | returnType;
    data[1] = 0x01; // config_code = acceleration
    uint16_t accelCode = (uint16_t)(accelRadS2 * 100.0f);
    data[2] = (accelCode >> 8) & 0xFF;
    data[3] = accelCode & 0xFF;
    return sendCANFrame(motorId_, data, 4);
}

// ---- 配置命令 (CAN ID 0x7FF) ----

bool ElectricGripper::setZero() {
    uint8_t data[4] = {};
    data[0] = (uint8_t)(motorId_ >> 8);
    data[1] = (uint8_t)(motorId_ & 0xFF);
    data[2] = 0x00;
    data[3] = 0x03; // MOTOR_ZERO_SET
    return sendConfigFrame(data, 4);
}

bool ElectricGripper::findZero() {
    stopMotion();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    clearError();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    uint8_t data[4] = {};
    data[0] = (uint8_t)(motorId_ >> 8);
    data[1] = (uint8_t)(motorId_ & 0xFF);
    data[2] = 0x00;
    data[3] = 0x10; // MOTOR_FIND_ZERO
    return sendConfigFrame(data, 4);
}

bool ElectricGripper::stopMotion() {
    uint8_t data[4] = {};
    data[0] = (uint8_t)(motorId_ >> 8);
    data[1] = (uint8_t)(motorId_ & 0xFF);
    data[2] = 0x00;
    data[3] = 0xFF; // Stop
    return sendConfigFrame(data, 4);
}

bool ElectricGripper::queryMotorId() {
    uint8_t data[4] = {};
    data[0] = 0xFF;
    data[1] = 0xFF;
    data[2] = 0x00;
    data[3] = 0x82; // MOTOR_QUERY_ID
    return sendConfigFrame(data, 4);
}

bool ElectricGripper::findMinLimit() {
    // 固定数据：速度控制 10rpm 慢速推向极限
    // CAN ID = motorId_, len=7, data: 40 41 20 00 00 00 32
    uint8_t data[7] = {0x40, 0x41, 0x20, 0x00, 0x00, 0x00, 0x32};
    return sendCANFrame(motorId_, data, 7);
}

bool ElectricGripper::sendPresetPosition(float positionDeg) {
    // 使用低速预设动作，电流限制沿用安全默认值。
    return sendPositionControl(positionDeg, 25.0f, DEFAULT_CURRENT_LIMIT);
}

// ---- ESP32 串口转 CAN 桥接 ----

bool ElectricGripper::hasSerialBridgeHandle() const {
    return serialBridge_ != nullptr && serialBridge_ != INVALID_HANDLE_VALUE;
}

bool ElectricGripper::configureSerialBridge(int baudRate) {
    if (!hasSerialBridgeHandle()) return false;

    DCB dcb;
    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);

    char dcbStr[64];
    snprintf(dcbStr, sizeof(dcbStr), "baud=%d parity=N data=8 stop=1", baudRate);
    if (!BuildCommDCBA(dcbStr, &dcb)) return false;

    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fAbortOnError = FALSE;

    if (!SetCommState(serialBridge_, &dcb)) return false;

    COMMTIMEOUTS timeouts;
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = 20;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 200;
    SetCommTimeouts(serialBridge_, &timeouts);
    SetupComm(serialBridge_, 4096, 4096);
    return true;
}

bool ElectricGripper::writeSerialBridgeLine(const std::string& line) {
    if (!hasSerialBridgeHandle()) return false;
    std::lock_guard<std::mutex> lock(serialBridgeMutex_);

    std::string payload = line;
    payload += "\r\n";

    DWORD written = 0;
    if (!WriteFile(serialBridge_, payload.data(), (DWORD)payload.size(), &written, NULL)) {
        return false;
    }
    FlushFileBuffers(serialBridge_);
    return written == payload.size();
}

bool ElectricGripper::waitForSerialBridgeReady(int timeoutMs) {
    if (!hasSerialBridgeHandle()) return false;

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    std::string text;
    bool ready = false;
    char buf[256];
    while (std::chrono::steady_clock::now() < deadline) {
        DWORD n = 0;
        if (ReadFile(serialBridge_, buf, sizeof(buf) - 1, &n, NULL) && n > 0) {
            buf[n] = '\0';
            text.append(buf, n);
            if (text.find("can_transceiver") != std::string::npos ||
                text.find("CAN ready") != std::string::npos ||
                text.find("[CAN]") != std::string::npos ||
                text.find("[CMD] status") != std::string::npos ||
                text.find("rxlog off") != std::string::npos) {
                ready = true;
            }

            size_t start = 0;
            while (true) {
                size_t pos = text.find_first_of("\r\n", start);
                if (pos == std::string::npos) break;
                if (pos > start) parseSerialBridgeLine(text.substr(start, pos - start));
                start = pos + 1;
            }
            if (start > 0) text.erase(0, start);

            if (ready) return true;
        } else {
            Sleep(20);
        }
    }
    return false;
}

bool ElectricGripper::waitForSerialBridgeMotorResponse(int timeoutMs) {
    if (!hasSerialBridgeHandle()) return false;

    uint64_t startUs = nowMicros();
    lastMotorResponseUs_.store(0);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    auto nextQuery = std::chrono::steady_clock::now();

    std::string text;
    char buf[256];
    while (std::chrono::steady_clock::now() < deadline) {
        auto now = std::chrono::steady_clock::now();
        if (now >= nextQuery) {
            // 优先使用固件内置 query_id 命令；再补一帧 raw，兼容早期固件只实现 raw/tx 的情况。
            writeSerialBridgeLine("query_id");
            writeSerialBridgeLine("raw 7FF FF FF 00 82");
            nextQuery = now + std::chrono::milliseconds(350);
        }

        DWORD n = 0;
        if (ReadFile(serialBridge_, buf, sizeof(buf) - 1, &n, NULL) && n > 0) {
            buf[n] = '\0';
            text.append(buf, n);

            size_t start = 0;
            while (true) {
                size_t pos = text.find_first_of("\r\n", start);
                if (pos == std::string::npos) break;
                if (pos > start) parseSerialBridgeLine(text.substr(start, pos - start));
                start = pos + 1;
            }
            if (start > 0) text.erase(0, start);

            uint64_t last = lastMotorResponseUs_.load();
            if (last >= startUs) return true;
        } else {
            Sleep(20);
        }
    }

    return false;
}

void ElectricGripper::markSerialBridgeAlive() {
    std::lock_guard<std::mutex> lock(stateMutex_);
    // 这里只表示 ESP32 串口桥接器仍在线，不代表电机 CAN 总线已连通。
    // 电机是否在线由 markMotorResponse()/lastMotorResponseUs_ 单独判断。
    fullState_.connected = true;
    state_.connected = true;
}

void ElectricGripper::markMotorResponse(uint32_t responseId, bool updateMotorId, const char* source) {
    auto now = nowMicros();
    if (updateMotorId && responseId >= 0x01 && responseId <= 0x7FE && responseId != motorId_) {
        motorId_ = responseId;
        if (usingSerialBridge_) {
            auto colon = portName_.find(':');
            if (colon != std::string::npos) portName_ = "ESP32-CAN:" + portName_.substr(colon + 1);
        } else {
            portName_ = "CAN:" + std::to_string(motorId_);
        }
        fprintf(stderr, "[电动夹爪] %s 确认电机ID: 0x%03X\n", source ? source : "回包", motorId_);
    }

    lastMotorResponseUs_.store(now);
    std::lock_guard<std::mutex> lock(stateMutex_);
    fullState_.connected = true;
    fullState_.hasData = true;
    fullState_.timestamp = now;
    state_.connected = true;
    state_.hasData = true;
    state_.timestamp = now;
}

void ElectricGripper::parseSerialBridgeLine(const std::string& line) {
    if (line.empty()) return;

    if (line.find("[TX]") != std::string::npos ||
        line.find("[CMD]") != std::string::npos ||
        line.find("[CAN]") != std::string::npos ||
        line.find("CAN ready") != std::string::npos) {
        markSerialBridgeAlive();
    }

    auto rxPos = line.find("[RX]");
    if (rxPos == std::string::npos) return;

    auto idPos = line.find("ID=0x", rxPos);
    auto dlcPos = line.find("DLC=", rxPos);
    auto dataPos = line.find("DATA=", rxPos);
    if (idPos == std::string::npos || dlcPos == std::string::npos || dataPos == std::string::npos) return;

    uint32_t id = 0;
    if (!parseHexId(line.substr(idPos + 5, 3), id)) return;

    int dlc = atoi(line.c_str() + dlcPos + 4);
    if (dlc < 0) dlc = 0;
    if (dlc > 8) dlc = 8;

    ECAN_CAN_OBJ obj = {};
    obj.ID = id;
    obj.DataLen = (BYTE)dlc;

    std::istringstream iss(line.substr(dataPos + 5));
    std::string token;
    int idx = 0;
    while (idx < dlc && iss >> token) {
        uint8_t value = 0;
        if (!parseHexByte(token, value)) break;
        obj.Data[idx++] = value;
    }
    obj.DataLen = (BYTE)idx;
    if (idx >= 2) {
        if (obj.ID == CAN_CONFIG_ID && obj.DataLen >= 5 &&
            obj.Data[0] == 0xFF && obj.Data[1] == 0xFF && obj.Data[2] == 0x01 && obj.Data[3] != 0x80) {
            uint16_t detectedId = ((uint16_t)obj.Data[3] << 8) | obj.Data[4];
            if (detectedId >= 0x01 && detectedId <= 0x7FE) {
                markMotorResponse(detectedId, true, "ESP32-CAN query_id 回包");
            }
        } else {
            if (obj.ID >= 0x01 && obj.ID <= 0x7FE && obj.ID != motorId_) {
                motorId_ = obj.ID;
                fprintf(stderr, "[电动夹爪] ESP32-CAN 从反馈学习到电机ID: 0x%03X\n", motorId_);
            }
            parseFeedback(obj);
        }
    }
}

void ElectricGripper::pollSerialBridge() {
    char buf[128];
    int readFailCount = 0;
    while (running_) {
        if (!hasSerialBridgeHandle()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        DWORD n = 0;
        BOOL readOk = ReadFile(serialBridge_, buf, sizeof(buf), &n, NULL);
        if (readOk && n > 0) {
            readFailCount = 0;
            for (DWORD i = 0; i < n; ++i) {
                char ch = buf[i];
                if (ch == '\r' || ch == '\n') {
                    if (!serialBridgeRxLine_.empty()) {
                        parseSerialBridgeLine(serialBridgeRxLine_);
                        serialBridgeRxLine_.clear();
                    }
                } else if (serialBridgeRxLine_.size() < 512) {
                    serialBridgeRxLine_.push_back(ch);
                } else {
                    serialBridgeRxLine_.clear();
                }
            }
        } else if (!readOk) {
            readFailCount++;
            if (readFailCount >= 20) {
                fprintf(stderr, "[电动夹爪] ESP32-CAN 串口桥接器已断开: %s\n", portName_.c_str());
                connected_ = false;
                state_.connected = false;
                fullState_.connected = false;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

// ---- 反馈帧解析 ----

void ElectricGripper::parseFeedback(ECAN_CAN_OBJ& obj) {
    if (obj.DataLen < 1) return;

    uint8_t returnType = (obj.Data[0] >> 5) & 0x07;
    uint8_t errorMsg = (obj.Data[0] & 0x1F) << 3;

    // 只把协议内可解析的反馈帧视为“电机真实在线”。这样可以避免短帧、噪声帧或其他 CAN 设备
    // 误触发 lastMotorResponseUs_，导致前端把电动夹爪显示成已连接。
    if ((returnType == 1 || returnType == 2 || returnType == 3) && obj.DataLen < 8) return;
    if (returnType == 5 && obj.DataLen < 6) return;
    if (returnType != 1 && returnType != 2 && returnType != 3 && returnType != 5) return;

    auto now = nowMicros();
    lastMotorResponseUs_.store(now);

    std::lock_guard<std::mutex> lock(stateMutex_);

    // 保存原始 CAN 帧，便于调试协议解析和设备反馈。
    memset(fullState_.rawFrame, 0, 8);
    fullState_.rawFrameLen = (uint8_t)(obj.DataLen > 8 ? 8 : obj.DataLen);
    memcpy(fullState_.rawFrame, obj.Data, fullState_.rawFrameLen);

    if (returnType == 1) {
        // 反馈类型 1：位置(mrad)、速度(rpm)、电流(A*100)、电机温度和 MOS 温度。
        uint16_t posRatio = (obj.Data[1] << 8) | obj.Data[2];
        int16_t posMRad = (int16_t)posRatio;
        fullState_.position = posMRad / 500.0f; // mrad -> rad
        fullState_.positionDeg = fullState_.position * 180.0f / PI;

        uint16_t velRatio = (obj.Data[3] << 4) | (obj.Data[4] >> 4);
        fullState_.velocity = (int16_t)velRatio; // rpm

        uint16_t curRatio = ((obj.Data[4] & 0x0F) << 8) | obj.Data[5];
        fullState_.current = (int16_t)curRatio / 100.0f; // A*100 -> A

        fullState_.motorTemp = (int8_t)obj.Data[6] - 50.0f;
        fullState_.mosTemp = (int8_t)obj.Data[7] - 50.0f;

        // 更新通用 GripperState，并按 0~4500 度行程归一化为 0~1。
        float normPos = fullState_.positionDeg / CLAW_SAFE_TRAVEL_DEG;
        if (normPos < 0.0f) normPos = 0.0f;
        if (normPos > 1.0f) normPos = 1.0f;
        state_.position = normPos;
        state_.hasData = true;
        state_.timestamp = now;

    } else if (returnType == 2) {
        // 反馈类型 2：位置(float32 度)、电流(A*100)和电机温度。
        uint32_t posBits = (obj.Data[1] << 24) | (obj.Data[2] << 16) | (obj.Data[3] << 8) | obj.Data[4];
        memcpy(&fullState_.positionDeg, &posBits, 4);
        fullState_.position = fullState_.positionDeg * PI / 180.0f;

        uint16_t curRatio2 = (obj.Data[5] << 8) | obj.Data[6];
        fullState_.current = (int16_t)curRatio2 / 100.0f;
        fullState_.motorTemp = (int8_t)obj.Data[7] - 50.0f;

        float normPos2 = fullState_.positionDeg / CLAW_SAFE_TRAVEL_DEG;
        if (normPos2 < 0.0f) normPos2 = 0.0f;
        if (normPos2 > 1.0f) normPos2 = 1.0f;
        state_.position = normPos2;
        state_.hasData = true;
        state_.timestamp = now;

    } else if (returnType == 3) {
        // 反馈类型 3：速度(float32 rpm)、电流(A*100)和电机温度。
        uint32_t velBits = (obj.Data[1] << 24) | (obj.Data[2] << 16) | (obj.Data[3] << 8) | obj.Data[4];
        memcpy(&fullState_.velocity, &velBits, 4);

        uint16_t curRatio3 = (obj.Data[5] << 8) | obj.Data[6];
        fullState_.current = (int16_t)curRatio3 / 100.0f;
        fullState_.motorTemp = (int8_t)obj.Data[7] - 50.0f;

    } else if (returnType == 5) {
        // 参数查询响应：根据返回类型更新完整状态缓存。
        uint8_t queryCode = obj.Data[1];
        if (obj.DataLen >= 6) {
            uint32_t valBits = (obj.Data[2] << 24) | (obj.Data[3] << 16) | (obj.Data[4] << 8) | obj.Data[5];
            float val;
            memcpy(&val, &valBits, 4);
            switch (queryCode) {
                case 1: fullState_.position = val; fullState_.positionDeg = val * 180.0f / PI; break;
                case 2: fullState_.velocity = val * 60.0f / (2.0f * PI); break; // rad/s -> rpm
                case 3: fullState_.current = val; break;
            }
        }
    }

    fullState_.errorCode = errorMsg;
    fullState_.connected = true;
    fullState_.hasData = true;
    fullState_.timestamp = now;
    fullState_.motorEnabled = (returnType > 0);
    state_.connected = true;
    state_.hasData = true;
    state_.timestamp = now;
}

// ---- 轮询线程 ----

void ElectricGripper::pollLoop() {
    if (usingSerialBridge_) {
        pollSerialBridge();
        return;
    }

    ECAN_CAN_OBJ recvBuf[10];
    int noDataCount = 0;

    while (running_) {
        if (!can_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // 检查接收缓冲区中的帧数
        ULONG bufCount = can_->getReceiveNum();
        int count = can_->receive(recvBuf, 10, 100); // 增大超时到100ms
        for (int i = 0; i < count; i++) {
            if (recvBuf[i].ID == CAN_CONFIG_ID && recvBuf[i].DataLen >= 2) {
                // 配置响应(0x7FF)：解析查询电机 ID 的返回帧。
                if (recvBuf[i].DataLen >= 5 && recvBuf[i].Data[0] == 0xFF && recvBuf[i].Data[1] == 0xFF
                    && recvBuf[i].Data[2] == 0x01 && recvBuf[i].Data[3] != 0x80) {
                    uint16_t detectedId = ((uint16_t)recvBuf[i].Data[3] << 8) | recvBuf[i].Data[4];
                    fprintf(stderr, "[电动夹爪] 查询到电机ID: 0x%03X\n", detectedId);
                    if (detectedId >= 0x01 && detectedId <= 0x7FE) {
                        markMotorResponse(detectedId, true, "GCAN query_id 回包");
                    }
                }
            } else if (recvBuf[i].ID != motorId_ && recvBuf[i].ID >= 0x01 && recvBuf[i].ID <= 0x7FE && recvBuf[i].DataLen >= 2) {
                // 收到非配置ID且非当前motorId的帧 -> 自动学习为实际电机ID
                motorId_ = recvBuf[i].ID;
                portName_ = "CAN:" + std::to_string(motorId_);
                fprintf(stderr, "[电动夹爪] 自动检测到电机ID: 0x%03X，后续指令将使用此ID\n", motorId_);
                parseFeedback(recvBuf[i]);
            } else if (recvBuf[i].ID == motorId_ && recvBuf[i].DataLen >= 2) {
                parseFeedback(recvBuf[i]);
            }
        }

        if (count == 0) {
            noDataCount++;
            if (noDataCount == 20) { // 约2秒
                fprintf(stderr, "[电动夹爪] 等待响应... (缓冲区帧数=%lu, receive返回=%d)\n",
                        (unsigned long)bufCount, count);
                noDataCount = 0;
            }
        } else {
            noDataCount = 0;
        }
    }
}
