// umi_driver_v1.cc - UMI 手动夹爪 V1 驱动实现
// 实现编码器读取、角度→距离转换、零位校准、CRC16 帧通信

#include "umi/umi_driver_v1.h"

#include <glog/logging.h>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <string>

#include "tool/crc.h"
#include "tool/time.h"

namespace hardware {
namespace {
// 串口通信参数
constexpr int kSerialBaudRate = 115200;   // 波特率
constexpr int kSerialTimeoutMs = 50;      // 读写超时（毫秒）

// UpdateCrc16 - 计算帧的 CRC16 校验值
// CRC = 0xFFFF - length - data（简化校验算法）
template <typename Frame>
void UpdateCrc16(Frame& frame) {
  frame.crc16 = 0xffff - frame.length - frame.data;
}

// RequestSingleRegFrame - 请求帧结构（PC → MCU）
// 格式：[Head(0xAA), Length, Data, CRC16_Low, CRC16_High]
struct RequestSingleRegFrame {
  uint8_t head = 0xaa;     // 帧头，固定 0xAA
  uint8_t length = 1;      // 数据长度
  uint8_t data;            // 命令字节：0x01=读位置, 0x02=校准, 0x03=校准并存储
  uint16_t crc16;          // CRC16 校验值
} __attribute__((packed));

// ReplyFrame - 响应帧模板（MCU → PC）
// 格式：[Head(0xAA), Length, Data[N], CRC16_Low, CRC16_High]
template <uint8_t N>
struct ReplyFrame {
  static_assert(N > 0);
  static constexpr uint8_t kDataSize = N;

  uint8_t head = 0xaa;     // 帧头
  uint8_t length = kDataSize;  // 数据长度
  uint8_t data[N];         // 数据负载（N 字节）
  uint16_t crc16;          // CRC16 校验值
} __attribute__((packed));

// 4 字节数据的响应帧（用于接收 float 类型的编码器角度）
using Reply4DataFrame = ReplyFrame<4>;
}  // namespace

// 构造函数：打开串口并执行零位校准
UmiDriverV1::UmiDriverV1(const std::string& uart_port, float max_distance) : max_distance_(max_distance) {
  LOG(INFO) << "Starting to open gripper encoder port: " << uart_port;
  CHECK(OpenPort(uart_port));
  CHECK(CalibrateZeroPosition());
}

// 析构函数：关闭串口
UmiDriverV1::~UmiDriverV1() {
  if (serial_.isOpen()) {
    serial_.close();
  }
}

// OpenPort - 打开并配置串口
bool UmiDriverV1::OpenPort(const std::string& uart_port) {
  serial_.setPort(uart_port);
  serial_.setBaudrate(kSerialBaudRate);
  serial::Timeout timeout_ms = serial::Timeout::simpleTimeout(kSerialTimeoutMs);
  serial_.setTimeout(timeout_ms);
  serial_.open();
  return serial_.isOpen();
}

// RequestResponse - 通用的请求-响应收发函数
// 1. 计算请求帧的 CRC16
// 2. 加锁发送请求帧
// 3. 读取响应帧并校验长度
// 4. 返回解析后的响应帧结构体
template <typename ResponseFrame, typename RequestFrame>
std::optional<ResponseFrame> UmiDriverV1::RequestResponse(RequestFrame& frame, bool print_hex) {
  constexpr int kRequsetFrameSize = sizeof(RequestFrame);
  constexpr int kResponseFrameize = sizeof(ResponseFrame);
  UpdateCrc16<RequestFrame>(frame);

  // 将请求帧序列化为字节数组
  uint8_t request[kRequsetFrameSize];
  std::memcpy(request, &frame, kRequsetFrameSize);
  if (print_hex) {
    std::stringstream ss;
    for (uint8_t req : request) {
      ss << std::hex << +req << " ";
    }
    LOG(INFO) << "Request: num of bytes " << kRequsetFrameSize << "; " << ss.str();
  }

  // 加锁发送请求并等待响应
  std::lock_guard<std::mutex> guard(mutex_);
  serial_.write(request, kRequsetFrameSize);

  uint8_t response[kResponseFrameize];
  size_t read_bytes_num = serial_.read(response, kResponseFrameize);

  // 校验响应长度
  if (read_bytes_num != kResponseFrameize) {
    LOG(WARNING) << "Received bytes " << read_bytes_num << " does not match expected number " << kResponseFrameize;
    serial_.flush();
    return std::nullopt;
  }
  if (print_hex) {
    std::stringstream ss;
    for (uint8_t res : response) {
      ss << std::hex << +res << " ";
    }
    LOG(INFO) << "Response: num of bytes " << kResponseFrameize << ", num of received bytes " << read_bytes_num << "; "
              << ss.str();
  }
  // 将响应字节拷贝到帧结构体
  ResponseFrame reply;
  std::memcpy(&reply, response, kResponseFrameize);
  return reply;
}

// Calibrate - 发送校准命令（CMD=0x02），不持久化
bool UmiDriverV1::Calibrate() {
  RequestSingleRegFrame frame;
  frame.data = 0x02;
  std::optional<Reply4DataFrame> reply = RequestResponse<Reply4DataFrame, RequestSingleRegFrame>(frame);
  if (!reply.has_value()) {
    return false;
  }

  return true;
}

// CalibrateAndStore - 发送校准并存储命令（CMD=0x03），写入 MCU Flash
bool UmiDriverV1::CalibrateAndStore() {
  RequestSingleRegFrame frame;
  frame.data = 0x03;
  std::optional<Reply4DataFrame> reply = RequestResponse<Reply4DataFrame, RequestSingleRegFrame>(frame);
  if (!reply.has_value()) {
    return false;
  }

  return true;
}

// GetEncoderPosition - 读取编码器原始角度值（度）
// 发送 CMD=0x01，接收 4 字节 float（编码器角度，单位：度）
std::optional<float> UmiDriverV1::GetEncoderPosition() {
  RequestSingleRegFrame frame;
  frame.data = 0x01;
  std::optional<Reply4DataFrame> reply = RequestResponse<Reply4DataFrame, RequestSingleRegFrame>(frame);
  if (!reply.has_value()) {
    return std::nullopt;
  }

  // 从响应帧的 data 字段解析出 float 类型的角度值
  float position = 0.0f;
  std::memcpy(&position, reply->data, 4);
  return position;
}

// GetRawPosition - 将编码器角度转换为夹爪开口距离（mm）
// 转换步骤：
// 1. 减去零位角度得到相对角度
// 2. 转换为弧度
// 3. 基于连杆几何计算开口距离：(16 - 10.5 + 55 * sin(θ)) * 2
std::optional<float> UmiDriverV1::GetRawPosition() {
  std::optional<float> raw_encoder_position = GetEncoderPosition();
  if (!raw_encoder_position.has_value()) {
    return std::nullopt;
  }

  float encoder_position_rad = (raw_encoder_position.value() - zero_encoder_position_) * M_PI / 180.0f;
  // 连杆几何公式：计算夹爪单侧行程，乘以 2 得到总开口距离
  float distance = (16.0f - 10.5f + 55.0f * std::sin(encoder_position_rad)) * 2.0f;

  VLOG(1) << std::fixed << std::setprecision(6) << "raw deg: " << raw_encoder_position.value()
          << ", zero deg: " << zero_encoder_position_ << ", rad: " << encoder_position_rad
          << ", position mm: " << distance;

  return distance;
}

// GetState - 获取当前夹爪状态（归一化位置 + 时间戳）
// 归一化公式：(当前距离 - 零位距离) / 最大距离，clamp 到 >= 0
UmiDriver::State UmiDriverV1::GetState() {
  std::optional<float> raw_position = GetRawPosition();
  State state;
  if (!raw_position.has_value()) {
    return state;
  }

  state.ts = common::Time::Now().ToDouble();
  state.position = std::max(0.0f, raw_position.value() - zero_position_) / max_distance_;
  return state;
}

// CalibrateZeroPosition - 零位校准
// 分两阶段：
// 1. 采样编码器原始角度 kNumZeroPosition 次，取平均值作为零位角度
// 2. 用零位角度计算对应的夹爪距离，取平均值作为零位距离
bool UmiDriverV1::CalibrateZeroPosition() {
  // 阶段 1：校准零位编码器角度
  float sum_zero_encoder = 0.0f;
  size_t num_zero_encoder = 0;
  for (size_t i = 0; i < kNumZeroPosition; i++) {
    std::optional<float> raw_encoder_position = GetEncoderPosition();
    if (raw_encoder_position.has_value()) {
      sum_zero_encoder += raw_encoder_position.value();
      ++num_zero_encoder;
    }
  }
  zero_encoder_position_ = sum_zero_encoder / num_zero_encoder;
  LOG(INFO) << "Calibrated zero encoder position: " << zero_encoder_position_ << " deg";

  // 阶段 2：校准零位夹爪距离
  float sum_zero_position = 0.0f;
  size_t num_zero_position = 0;
  for (size_t i = 0; i < kNumZeroPosition; i++) {
    std::optional<float> raw_position = GetRawPosition();
    if (raw_position.has_value()) {
      sum_zero_position += raw_position.value();
      ++num_zero_position;
    }
  }
  zero_position_ = sum_zero_position / num_zero_position;
  LOG(INFO) << "Calibrated zero position: " << zero_position_ << " mm";

  return true;
}
}  // namespace hardware
