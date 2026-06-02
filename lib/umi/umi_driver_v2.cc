// umi_driver_v2.cc - UMI 手动夹爪 V2 驱动实现
// 实现后台轮询线程、帧同步、LED 控制、串口通信
// V2 协议：MCU→PC [0x0A, Encoder[4], Btn1, Btn2, 0x0A]
//          PC→MCU [0x0A, Cmd, R, G, B, Bright, 0x0B]

#include "umi/umi_driver_v2.h"

#include <glog/logging.h>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <string>

#include "tool/time.h"

namespace hardware {
namespace {
// 串口通信参数
constexpr int kSerialBaudRate = 115200;   // 波特率
constexpr int kSerialTimeoutMs = 50;      // 读写超时（毫秒）

// MCU→PC 状态包格式：[Header(0x0A), Encoder[4], Button1, Button2, Tail(0x0A)]
// Encoder[4]: float 类型编码器位置，小端序
// Button1/Button2: 按键状态
constexpr size_t kResponsePacketSize = 8;
// PC→MCU 命令包格式：[Header(0x0A), Command, R, G, B, Brightness, Tail(0x0B)]
// Command: 0x01=启动数据流, 0x02=停止数据流, 0x00=设置LED
constexpr size_t kCommandPacketSize = 7;

// 启动数据流命令
constexpr uint8_t kStartCom[kCommandPacketSize] = {0x0A, 0x01, 0x00, 0x00, 0x00, 0x00, 0x0B};
// 停止数据流命令
constexpr uint8_t kStopCom[kCommandPacketSize] = {0x0A, 0x02, 0x00, 0x00, 0x00, 0x00, 0x0B};

// SyncFrame - 帧同步函数
// 当检测到帧头/帧尾不匹配时，逐字节滑动窗口寻找有效帧
// input: 存放当前读取的 kResponsePacketSize 字节（会被覆盖为同步后的数据）
// serial: 串口对象
// 返回 true 表示同步成功，false 表示同步失败
bool SyncFrame(uint8_t* input, serial::Serial& serial) {
  constexpr int kBufferCapacity = 4 * kResponsePacketSize;
  uint8_t buffer[kBufferCapacity];
  // 将当前已读取的数据拷贝到缓冲区开头
  std::memcpy(buffer, input, kResponsePacketSize);
  // 逐字节读取，在缓冲区中滑动窗口寻找有效帧
  for (int i = 0; i < kBufferCapacity - kResponsePacketSize; ++i) {
    int n = serial.read(&buffer[kResponsePacketSize + i], 1);
    if (n != 1) {
      LOG(ERROR) << "Wrong number of bytes received: expected=1 actual=" << n;
      return false;
    }
    // 滑动窗口：每次向前移动 1 字节
    std::memcpy(input, &buffer[i + 1], kResponsePacketSize);
    // 检查帧头和帧尾是否匹配 0x0A
    if (input[0] != 0x0A || input[kResponsePacketSize - 1] != 0x0A) {
      VLOG(1) << "Successfully synced packet frame after reading " << i + 1 << " bytes";
      return true;
    }
  }
  LOG(ERROR) << "Failed to sync packet frame";
  return false;
}
}  // namespace

// 构造函数：重启驱动（内部调用 Restart 完成初始化）
UmiDriverV2::UmiDriverV2(const std::string& uart_port) {
  uart_port_ = uart_port;
  CHECK(Restart());
}

// 析构函数：停止后台线程并关闭串口
UmiDriverV2::~UmiDriverV2() { Stop(); }

// Restart - 重启驱动
// 1. 停止现有线程和串口
// 2. 重新打开串口并初始化
// 3. 如果初始化失败，等待 0.5 秒后重试一次
// 4. 启动后台轮询线程
bool UmiDriverV2::Restart() {
  Stop();
  if (!OpenPort(uart_port_)) {
    return false;
  }

  if (!Initialize()) {
    LOG(WARNING) << "Failed to initialize UmiDriverV2. Reopen again";
    serial_.close();
    common::Sleep(common::Seconds(0.5));

    if (!OpenPort(uart_port_) || !Initialize()) {
      return false;
    }
  }

  thread_ = std::thread(&UmiDriverV2::Run, this);
  LOG(INFO) << "Started with uart_port=" << uart_port_;
  return true;
}

// Initialize - 初始化硬件通信
// 1. 发送停止命令清空 MCU 端可能的残留数据
// 2. 清空串口输入缓冲区
// 3. 发送启动命令，MCU 开始持续发送状态数据
// 4. 设置 LED 为低亮度 (10,10,10) 表示已连接
// 5. 尝试读取第一个状态包，验证通信是否正常
bool UmiDriverV2::Initialize() {
  serial_.write(kStopCom, kCommandPacketSize);
  common::Sleep(common::Seconds(0.1));
  serial_.flushInput();
  serial_.write(kStartCom, kCommandPacketSize);
  SetLed(10, 10, 10);

  uint8_t buffer[kResponsePacketSize];
  size_t read_bytes_num = serial_.read(buffer, kResponsePacketSize);
  if (read_bytes_num != kResponsePacketSize) {
    LOG(ERROR) << "Wrong number of bytes received: expected=" << kResponsePacketSize << " actual=" << read_bytes_num;
    return false;
  }
  return true;
}

// Stop - 停止后台线程并关闭串口
// 1. 设置 running_ = false 并等待线程结束
// 2. 关闭 LED、发送停止命令、关闭串口
void UmiDriverV2::Stop() {
  if (running_) {
    running_ = false;
    if (thread_.joinable()) {
      thread_.join();
    }
  }
  if (serial_.isOpen()) {
    SetLed(0, 0, 0, 0);
    serial_.write(kStopCom, kCommandPacketSize);
    serial_.close();
  }
}

// OpenPort - 打开并配置串口
bool UmiDriverV2::OpenPort(const std::string& uart_port) {
  serial_.setPort(uart_port);
  serial_.setBaudrate(kSerialBaudRate);
  serial::Timeout timeout_ms = serial::Timeout::simpleTimeout(kSerialTimeoutMs);
  serial_.setTimeout(timeout_ms);
  serial_.open();
  return serial_.isOpen();
}

// GetState - 获取最新状态（消费式读取）
// 返回当前缓存的 state_，然后将 ts 清零
// 如果下次调用时后台线程还没收到新数据，返回的 state.ts 会是 0
UmiDriver::State UmiDriverV2::GetState() {
  std::lock_guard<std::mutex> guard(mutex_);
  auto cur_state = state_;
  state_.ts = 0.0;
  return cur_state;
}

// SetLedImpl - 发送 LED 控制命令
// 命令格式：[0x0A, 0x00, R, G, B, Brightness, 0x0B]
void UmiDriverV2::SetLedImpl(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness) {
  static uint8_t buffer[kCommandPacketSize] = {0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0B};
  buffer[2] = r;
  buffer[3] = g;
  buffer[4] = b;
  buffer[5] = brightness;
  serial_.write(buffer, kCommandPacketSize);
}

// Run - 后台轮询线程主循环
// 持续从串口读取 8 字节状态包：
// [0x0A, position_float[4], button1, button2, 0x0A]
// 解析后直接 memcpy 到 state_ 的前 6 字节（position + button1 + button2）
// 如果帧头/帧尾不匹配，调用 SyncFrame 重新同步
void UmiDriverV2::Run() {
  running_ = true;
  while (running_) {
    uint8_t buffer[kResponsePacketSize];
    size_t read_bytes_num = serial_.read(buffer, kResponsePacketSize);
    if (read_bytes_num != kResponsePacketSize) {
      LOG(ERROR) << "Wrong number of bytes received: expected=" << kResponsePacketSize << " actual=" << read_bytes_num;

      continue;
    }

    // 校验帧头 (0x0A) 和帧尾 (0x0A)
    if (buffer[0] != 0x0A || buffer[kResponsePacketSize - 1] != 0x0A) {
      if (!SyncFrame(buffer, serial_)) {
        break;
      }
    }
    // 加锁更新状态：时间戳 + 从 buffer[1..6] 拷贝 position + buttons
    std::lock_guard<std::mutex> guard(mutex_);
    state_.ts = common::Time::Now().ToDouble();
    // 直接拷贝 6 字节到 state_：position(4) + button1(1) + button2(1)
    std::memcpy(&state_, &buffer[1], kResponsePacketSize - 2);
  }
  running_ = false;
}
}  // namespace hardware
