// umi_driver_v2.h - UMI 手动夹爪 V2 驱动（编码器 + 按钮 + LED 版本）
// V2 相比 V1 增加了：两个按键读取、LED RGB 控制、后台轮询线程
// 通信协议：自定义二进制帧（Header=0x0A, Tail=0x0A/0x0B）

#pragma once

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "serial/serial.h"
#include "umi/umi_driver.h"

namespace hardware {

// UmiDriverV2 - V2 版本驱动（配备编码器 + 按钮 + LED）
// 工作流程：打开串口 → 发送启动命令 → 后台线程持续轮询状态
// 后台线程持续从串口读取 8 字节状态包并更新 state_
class UmiDriverV2 : public UmiDriver {
 public:
  // 构造函数：打开串口、初始化硬件、启动后台轮询线程
  // uart_port: 串口设备路径（如 /dev/ttyUSB0）
  UmiDriverV2(const std::string& uart_port);
  ~UmiDriverV2();

  // GetState - 获取最新状态并清零时间戳（消费式读取）
  // 返回后 state_.ts 会被清零，下次调用如果无新数据则 ts=0
  State GetState() final;

  // Restart - 重启驱动（停止线程 → 重新打开串口 → 重新初始化）
  bool Restart() final;

 private:
  // SetLedImpl - 发送 LED 控制命令到 MCU
  // 命令格式：[0x0A, 0x00, R, G, B, Brightness, 0x0B]
  void SetLedImpl(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness = 100) final;

  // OpenPort - 打开并配置串口（115200 波特率，50ms 超时）
  bool OpenPort(const std::string& uart_port);

  // Initialize - 初始化硬件通信
  // 1. 发送停止命令清空缓冲区
  // 2. 发送启动命令开始数据流
  // 3. 设置 LED 为低亮度
  // 4. 验证是否能收到第一个数据包
  bool Initialize();

  // Run - 后台轮询线程主循环
  // 持续从串口读取 8 字节状态包，解析编码器位置和按键状态
  // 如果帧头/帧尾不匹配则调用 SyncFrame 重新同步
  void Run();

  // Stop - 停止后台线程并关闭串口
  void Stop();

  serial::Serial serial_;              // 串口通信对象

  std::string uart_port_;              // 串口设备路径（用于重启时重新打开）
  std::atomic<bool> running_{false};   // 后台线程运行标志
  std::thread thread_;                  // 后台轮询线程
  std::mutex mutex_;                    // state_ 访问互斥锁
  State state_;                         // 最新夹爪状态（由后台线程更新）
};
}  // namespace hardware
