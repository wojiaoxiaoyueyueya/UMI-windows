// umi_driver.h - UMI 手动夹爪驱动基类
// 定义夹爪驱动的通用接口：状态读取、LED 控制、重启
// V1（仅编码器）和 V2（编码器 + 按钮 + LED）均继承此基类

#pragma once

#include <string>
#include <vector>

namespace hardware {

// UmiDriver - UMI 手动夹爪驱动基类（接口类）
// 提供统一的状态查询和 LED 控制接口，子类实现具体硬件通信
class UmiDriver {
 public:
  // State - 夹爪状态结构体
  // position: 夹爪归一化位置 [0.0, 1.0]，0=零位，1=最大开口
  // button1/button2: 按键状态，255 表示无效/未连接
  // ts: 时间戳（秒）
  struct State {
    // position 和 button1 必须放在前面，因为 V2 的 Run() 中
    // 使用 std::memcpy 直接从串口缓冲区拷贝到 State 结构体
    float position = 0.0f;
    // 255 means invalid.
    uint8_t button1 = 255;
    uint8_t button2 = 255;

    double ts = 0.0;
  };

  virtual ~UmiDriver() {}

  // GetState - 获取夹爪当前状态（位置、按键、时间戳）
  virtual State GetState() = 0;

  // Restart - 重启夹爪驱动（重新初始化串口和硬件）
  virtual bool Restart() { return false; }

  // SetLed - 设置 LED 颜色（vector 形式），brightness 0~255
  void SetLed(const std::vector<uint8_t>& rgb, uint8_t brightness = 100) { SetLed(rgb[0], rgb[1], rgb[2], brightness); }

  // SetLed - 设置 LED 颜色（独立 RGB 分量），brightness 0~255
  // 内部做了去重：如果颜色与上次相同则跳过，减少串口通信开销
  void SetLed(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness = 100) {
    // Ignore same led command to reduce communication cost.
    if (rgb_.empty() || rgb_[0] != r || rgb_[1] != g || rgb_[2] != b) {
      SetLedImpl(r, g, b, brightness);
    }
    rgb_ = {r, g, b};
  }

 private:
  // SetLedImpl - 实际发送 LED 控制命令（由子类实现）
  virtual void SetLedImpl(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness) {}

  // 上次设置的 RGB 值，用于去重判断
  std::vector<uint8_t> rgb_;
};
}  // namespace hardware
