// test_umi_driver_v2.cc - UMI 手动夹爪 V2 驱动测试
// 测试 V2 驱动的位置读取、按键检测、LED 控制，可选配合海康相机测试
// 用法：test_umi_driver_v2 <uart_port> [camera]
// 示例：test_umi_driver_v2 /dev/ttyUSB0           → 仅读取夹爪状态
//       test_umi_driver_v2 /dev/ttyUSB0 camera    → 同时读取夹爪和相机

#include "umi/umi_driver_v2.h"

#include <glog/logging.h>
#include <gtest/gtest.h>

#include <csignal>
#include <iomanip>
#include <optional>
#include <vector>

#include "hikvision/hikvision_capture.h"
#include "tool/time.h"

namespace {
// 全局配置：g_configs[0]=串口路径, g_configs[1]=是否启用相机（可选）
std::vector<std::string> g_configs;
// Ctrl+C 信号标志，用于优雅退出测试循环
volatile std::sig_atomic_t g_sigint_status;
}  // namespace

// Test - 持续读取并打印夹爪状态，可选同时打印相机帧率
// LED 每 2 秒自动切换颜色（红→绿→蓝循环），用于验证 LED 控制功能
TEST(UmiDriverV2, Test) {
  hardware::UmiDriverV2 driver(g_configs[0]);
  std::unique_ptr<hardware::HikvisionCapture<cv::Mat>> cam;
  // 如果命令行传了第二个参数，则同时打开海康相机
  if (g_configs.size() > 1) {
    hardware::HikvisionCaptureConfig config;
    cam = std::make_unique<hardware::HikvisionCapture<cv::Mat>>(config);
    cam->StartDevice();
  }
  auto start_ts = common::Time::Now();
  int index = 0;
  int64_t color_time = 0;
  double fps = 0.0;
  // 主循环：持续读取状态直到 Ctrl+C
  while (!g_sigint_status) {
    auto state = driver.GetState();
    if (cam != nullptr) {
      // 如果启用了相机，同时打印帧率和夹爪状态
      auto main_color_image_ptr = cam->GetColorImage();
      if (main_color_image_ptr != nullptr) {
        fps = 1.0e6 / (main_color_image_ptr->first - color_time);
        color_time = main_color_image_ptr->first;
        LOG_EVERY_T(INFO, 0.2) << std::fixed << std::setprecision(2) << "fps: " << fps
                               << "; position: " << state.position << "; button1: " << static_cast<int>(state.button1)
                               << "; button2: " << static_cast<int>(state.button2) << std::fixed
                               << std::setprecision(2);
      }
    } else {
      // 仅打印夹爪状态（位置 + 按键）
      LOG_EVERY_T(INFO, 0.2) << std::fixed << std::setprecision(2) << "position: " << state.position
                             << "; button1: " << static_cast<int>(state.button1)
                             << "; button2: " << static_cast<int>(state.button2);
    }

    // 每 2 秒切换 LED 颜色（红→绿→蓝循环）
    if (common::Time::Now() - start_ts > common::Seconds(2.0)) {
      std::vector<uint8_t> rgb = {0, 0, 0};
      rgb[index % 3] = 100;
      driver.SetLed(rgb[0], rgb[1], rgb[2]);
      start_ts = common::Time::Now();
      ++index;
    }
  }
}

// main - 解析命令行参数、注册信号处理、运行测试
int main(int argc, char** argv) {
  if (argc < 2) {
    LOG(ERROR) << "Usage: " << argv[0] << " <uart_port> <camera>";
    return 1;
  }
  // 注册 SIGINT 信号处理，Ctrl+C 时优雅退出
  std::signal(SIGINT, [](int signal) { g_sigint_status = signal; });

  if (argc > 2) {
    g_configs = {argv[1], argv[2]};
  } else {
    g_configs = {argv[1]};
  }

  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
