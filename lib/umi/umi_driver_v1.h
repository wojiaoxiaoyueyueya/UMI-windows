// umi_driver_v1.h - UMI 手动夹爪 V1 驱动（仅编码器版本）
// 通过 UART 串口与夹爪编码器 MCU 通信，读取编码器角度并转换为夹爪开口距离
// 通信协议：自定义二进制帧，CRC16 校验

#pragma once

#include <array>
#include <mutex>
#include <optional>
#include <string>

#include "serial/serial.h"
#include "umi/umi_driver.h"

namespace hardware {

// UmiDriverV1 - V1 版本驱动（仅配备夹爪编码器，无按钮和 LED）
// 工作流程：打开串口 → 零位校准 → 按需读取编码器位置
class UmiDriverV1 : public UmiDriver {
 public:
  // 零位校准时采样的次数，取多次平均值以提高精度
  static constexpr uint16_t kNumZeroPosition = 100;

  // 构造函数：打开串口并执行零位校准
  // uart_port: 串口设备路径（如 /dev/ttyUSB0）
  // max_distance: 夹爪最大开口距离（mm），用于归一化
  UmiDriverV1(const std::string& uart_port, float max_distance);
  ~UmiDriverV1();

  // Calibrate - 发送校准命令（CMD=0x02），不保存到 MCU Flash
  bool Calibrate();
  // CalibrateAndStore - 发送校准并存储命令（CMD=0x03），写入 MCU Flash，需重启生效
  bool CalibrateAndStore();
  // GetState - 获取当前夹爪状态（归一化位置 + 时间戳）
  State GetState() final;

 private:
  // OpenPort - 打开并配置串口（115200 波特率，50ms 超时）
  bool OpenPort(const std::string& uart_port);
  // GetEncoderPosition - 从 MCU 读取编码器原始角度值（度）
  std::optional<float> GetEncoderPosition();
  // GetRawPosition - 将编码器角度转换为夹爪开口距离（mm）
  // 转换公式基于连杆几何：(16 - 10.5 + 55 * sin(θ)) * 2
  std::optional<float> GetRawPosition();
  // CalibrateZeroPosition - 零位校准：采样 kNumZeroPosition 次取平均
  // 记录零位时的编码器角度和对应的夹爪距离
  bool CalibrateZeroPosition();

  // RequestResponse - 发送请求帧并等待响应帧（模板化的通用收发函数）
  // 自动计算 CRC16、发送请求、读取响应，返回解析后的响应帧
  template <typename ResponseFrame, typename RequestFrame>
  std::optional<ResponseFrame> RequestResponse(RequestFrame& frame, bool print_hex = false);

  std::mutex mutex_;                // 串口访问互斥锁
  serial::Serial serial_;           // 串口通信对象
  float zero_encoder_position_ = 0.0f;  // 零位时的编码器角度（度）
  float zero_position_ = 0.0f;          // 零位时的夹爪距离（mm）
  float max_distance_ = 0.0f;           // 夹爪最大开口距离（mm）
};
}  // namespace hardware
