// test_umi_driver_v1.cc - UMI 手动夹爪 V1 驱动测试
// 测试 V1 驱动的零位校准和编码器位置读取功能
// 用法：test_umi_driver_v1 <uart_port> [is_calibrate_and_store]
// 示例：test_umi_driver_v1 /dev/ttyUSB0       → 持续读取位置
//       test_umi_driver_v1 /dev/ttyUSB0 1     → 执行校准并存储

#include "umi/umi_driver_v1.h"

#include <glog/logging.h>
#include <gtest/gtest.h>

#include <optional>
#include <vector>

#include "tool/time.h"

namespace {
// 全局配置：g_configs[0]=串口路径, g_configs[1]=是否执行校准并存储
std::vector<std::string> g_configs;

// CalibrateAndStore - 发送校准并存储命令，完成后需手动重启编码器 MCU
void CalibrateAndStore(hardware::UmiDriverV1& driver) {
  common::Time start = common::Time::Now();
  driver.CalibrateAndStore();
  common::Sleep(common::Seconds(1));
  LOG(INFO) << "Finished calibrate and store, please reboot the gripper encoder!";
}

// ReadPosition - 持续读取并打印编码器位置（2ms 间隔）
void ReadPosition(hardware::UmiDriverV1& driver) {
  common::Time start = common::Time::Now();

  while (true) {
    hardware::UmiDriver::State state = driver.GetState();
    LOG(INFO) << "Current position: " << state.position << " mm";
    common::Sleep(common::Milliseconds(2));
  }
}
}  // namespace

// CalibrateOrReadPosition - 根据命令行参数选择校准或读取模式
TEST(UmiDriverV1, CalibrateOrReadPosition) {
  // max_distance=90.0mm 是夹爪的最大开口距离
  hardware::UmiDriverV1 driver(g_configs[0], 90.0f);

  if (g_configs[1] == "1") {
    CalibrateAndStore(driver);
  } else {
    ReadPosition(driver);
  }
}

// main - 解析命令行参数并运行测试
int main(int argc, char** argv) {
  if (argc < 2) {
    LOG(ERROR) << "Usage: " << argv[0] << " <uart_port> <is_calibrate_and_store>";
    return 1;
  } else if (argc == 3) {
    g_configs = {argv[1], argv[2]};
  } else {
    // 默认不校准，只读取位置
    g_configs = {argv[1], "0"};
  }

  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
