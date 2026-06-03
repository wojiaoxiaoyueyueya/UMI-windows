// HttpServer.hpp - HTTP 服务器头文件（Windows 版，多摄像头）
// 基于 cpp-httplib 实现
// 功能：多路 MJPEG 视频流推送、IMU/夹爪数据、数据录制与转换、静态文件服务

#pragma once

#include <mutex>
#include <string>
#include <vector>
#include <map>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <set>
#include <fstream>
#include <list>
#include <functional>
#include <opencv2/opencv.hpp>

#include "UmiGripper.hpp"
#include "IGripper.hpp"
#include "ElectricGripper.hpp"
#include "Config.hpp"
#include "DeviceManager.hpp"

namespace httplib { class Server; }

struct StreamState {
    std::mutex mutex;
    cv::Mat              mat;
    std::vector<uchar>   rawJpegBuf;
    std::vector<uchar>   encodedJpegBuf;
    uint64_t             timestamp = 0;
    uint64_t             encodedTimestamp = 0;
    uint64_t             lastSentTimestamp = 0;
    bool                 hasData = false;
    bool                 hasRawJpeg = false;
    int                  encodeFps = 0;
    int                  _encodeCount = 0;
    uint64_t             _encodeFpsTime = 0;
    // 采集帧计数器：始终统计原始采集数量，不受 MJPEG 页面预览开关影响。
    uint64_t             captureCount = 0;
    double               captureFps = 0;
    uint64_t             _captureFpsTime = 0;
    uint64_t             _capturePrevCount = 0;
};

// 多摄像头流状态：每个 slot 包含 color + depth + irLeft + irRight + pointcloud
struct CameraStreamStates {
    StreamState color;
    StreamState depth;
    StreamState irLeft;
    StreamState irRight;
    // 点云状态（不使用 MJPEG 编码，直接存 float 数据）
    std::mutex              pointCloudMutex;
    std::vector<float>      pointCloudData;
    int                     pointCloudWidth = 0;
    int                     pointCloudHeight = 0;
    uint64_t                pointCloudTimestamp = 0;
    bool                    pointCloudHasData = false;
    // 点云采集帧率统计：用于前端显示真实点云生产速度。
    uint64_t                pointCloudCaptureCount = 0;
    double                  pointCloudCaptureFps = 0;
    uint64_t                _pcCaptureFpsTime = 0;
    uint64_t                _pcCapturePrevCount = 0;
};

struct GripperWebState {
    std::mutex mutex;
    float  position = 0.0f;
    uint8_t button1 = 255;
    uint8_t button2 = 255;
    uint64_t timestamp = 0;
    bool   hasData = false;
    // 夹爪采样帧率统计：用于前端显示真实夹爪数据刷新速度。
    uint64_t captureCount = 0;
    double   captureFps = 0;
    uint64_t _captureFpsTime = 0;
    uint64_t _capturePrevCount = 0;
};

struct ElectricGripperWebState {
    std::mutex mutex;
    float positionDeg = 0.0f;
    float velocity = 0.0f;
    float current = 0.0f;
    float motorTemp = 0.0f;
    float mosTemp = 0.0f;
    uint8_t errorCode = 0;
    bool motorEnabled = false;
    bool hasData = false;
    bool connected = false;
    uint64_t timestamp = 0;
    uint8_t rawFrame[8] = {};
    uint8_t rawFrameLen = 0;
};

struct DeviceInfoState {
    std::mutex mutex;
    std::string name;
    int pid = 0;
    int vid = 0;
    std::string serialNumber;
    std::string firmwareVersion;
    uint64_t lastUpdateTime = 0;
    bool hasData = false;
};

struct PoseState {
    std::mutex mutex;
    double tx = 0, ty = 0, tz = 0;
    double qx = 0, qy = 0, qz = 0, qw = 1;
    double roll = 0, pitch = 0, yaw = 0;
    uint64_t timestamp = 0;
    bool hasData = false;
};

struct RecordingState {
    // 单个槽位的录制状态；map 的 key 是实际目录名，例如 "1Left-umi"。
    struct SlotState {
        std::string slotDir;          // absolute path to slot folder
        std::string position;         // "left", "right", "head"
        std::string deviceType;       // "hikvision" or "orbbec"

        // 按视频流类型统计帧数，例如 color/depth/ir-left/ir-right。
        std::map<std::string, uint64_t> frameCount;  // "color"->N, "depth"->N, "ir-left"->N, ...
        uint64_t gripperCount = 0;
        float gripperMinPos = 1e9f;
        float gripperMaxPos = -1e9f;

        // 电动夹爪统计信息：用于 metadata.json 汇总和前端展示。
        uint64_t egCount = 0;
        float egMinPos = 1e9f;
        float egMaxPos = -1e9f;
        float egMaxVel = 0.0f;
        float egMaxCur = 0.0f;

        // 每路视频独立一个 VideoWriter；严格同步模式下主要在停止录制后的收尾阶段使用。
        std::map<std::string, cv::VideoWriter> videoWriters;   // "color"/"depth"/"ir" -> writer
        std::map<std::string, std::ofstream> timestampCsvs;    // "color"/"depth"/"ir" -> CSV

        // 压缩帧缓存：录制期间缓存 JPEG，停止时用最终会话时长计算 FPS，保证多路视频时长统一。
        std::map<std::string, std::vector<std::vector<uint8_t>>> compressedFrames;  // streamType -> JPEG buffers
        // 全量时间戳：录制期间持续追加，finalize 时计算精确 FPS 和时长。
        std::map<std::string, std::vector<uint64_t>> allTimestamps;  // streamType -> all timestamps

        struct StreamFormatInfo {
            std::string format;
            int width = 0;
            int height = 0;
            uint64_t firstTimestamp = 0;
        };
        std::map<std::string, StreamFormatInfo> streamInfo;  // "color"/"depth"/"ir"

        // 夹爪 CSV 文件句柄，同一槽位的手动/电动夹爪数据都写入 gripper_data/gripper.csv。
        std::ofstream gripperCsvFile;

        // 时间戳同步信息：记录第一帧视频时间，便于 metadata 描述会话时间基准。
        uint64_t firstVideoDeviceTimestamp = 0;
        bool hasFirstVideoTimestamp = false;
        int videoFpsMeasured = 0;

        // 点云录制计数和抽帧间隔，避免每帧点云都落盘导致磁盘压力过大。
        uint64_t pcFrameCount = 0;
        uint64_t pcSaveInterval = 5; // save every N frames
    };

    std::mutex mutex;
    bool isRecording = false;
    bool finalizing = false;
    std::string sessionPath;
    std::string sessionId;
    std::string baseDir;
    uint64_t startTime = 0;
    uint64_t stopTime = 0;
    double fps = 0.0;
    bool fastSaveMode = false;
    std::set<std::string> selectedStreams; // selected stream keys e.g. "left-color"
    std::vector<std::string> warnings; // recording warnings for frontend display

    // 反向槽位映射：后端真实槽位名 -> 前端显示位置名。
    // 开始录制时由前端 slotMapping 填充，用于保证目录命名和页面选择一致。
    std::map<std::string, std::string> backendToDisplay;

    // 目录名 -> 槽位录制状态，例如 "Left-umi" 对应左手录制目录。
    std::map<std::string, SlotState> slots;

    // 显示位置名转换为录制目录名，统一 left/right/head 的文件夹命名。
    static std::string positionToFolder(const std::string& pos) {
        if (pos == "left")  return "Left-umi";
        if (pos == "right") return "Right-umi";
        if (pos == "head")  return "Head-umi";
        return pos + "-umi";
    }
};

struct ConvertState {
    std::mutex mutex;
    bool converting = false;
    int totalSessions = 0;
    int completedSessions = 0;
    std::string currentSession;
    std::string currentStep;
    std::string error;
    std::vector<std::string> convertedSessions;
    std::vector<std::string> skippedSessions;
};

// 保存任务队列：每次 stopRecording 入队一个 FinalizeTask，
// finalizeWorker 按顺序逐个处理，实现"第一个保存完第二个才开始"。
struct FinalizeTask {
    std::string sessionId;
    std::string sessionPath;
    std::string baseDir;
    uint64_t startTime = 0;
    uint64_t stopTime = 0;
    bool fastSaveMode = false;
    std::map<std::string, RecordingState::SlotState> slots;
    enum Status { PENDING, RUNNING, COMPLETED, CANCELLED };
    Status status = PENDING;
};

class HttpServer {
public:
    HttpServer(const Config& cfg, const std::string& frontendDir);
    ~HttpServer();

    void start();
    void stop();
    void setDeviceManager(DeviceManager* mgr) { deviceManager_ = mgr; }

    // 单摄像头（向后兼容）
    void updateColorFrame(const cv::Mat& mat);
    void updateGripperData(const std::string& slot, float position, uint8_t button1, uint8_t button2, uint64_t timestamp);
    void recordGripper(const std::string& slot, float position, uint8_t button1, uint8_t button2, uint64_t timestampUs);
    void recordElectricGripper(const std::string& slot, float positionDeg, float velocity, float current,
                               float motorTemp, float mosTemp, uint8_t errorCode, uint64_t timestampUs);
    void setUmiGripper(const std::string& slot, UmiGripper* mgr) { umiGrippers_[slot] = mgr; gripperWebStates_[slot]; }
    void setElectricGripper(const std::string& slot, ElectricGripper* gripper) { electricGrippers_[slot] = gripper; electricGripperWebStates_[slot]; }
    void updateElectricGripperData(const std::string& slot, const ElectricGripperFullState& state);
    ElectricGripper* getElectricGripper(const std::string& slot) const;
    void updateDeviceInfo(const std::string& name, int pid, int vid,
                          const std::string& serialNumber, const std::string& firmwareVersion);

    // 多摄像头
    void updateColorFrame(const std::string& slot, const cv::Mat& mat);
    void updateDepthFrame(const std::string& slot, const cv::Mat& mat);
    void updateIRLeftFrame(const std::string& slot, const cv::Mat& mat);
    void updateIRRightFrame(const std::string& slot, const cv::Mat& mat);
    void updatePointCloudData(const std::string& slot, const std::vector<float>& points, int width, int height);
    void updatePoseData(double tx, double ty, double tz,
                        double qx, double qy, double qz, double qw,
                        double roll, double pitch, double yaw, uint64_t timestamp);

    bool isStreamActive(const std::string& stream) const;
    void setStreamActive(const std::string& stream, bool active);
    bool isStreamActive(const std::string& slot, const std::string& stream) const;
    void setStreamActive(const std::string& slot, const std::string& stream, bool active);
    bool isRecording() const;
    void tickCaptureFrame(const std::string& slot, const std::string& streamType);
    void tickPointCloudFrame(const std::string& slot);
    void recordFrame(const std::string& slot, const std::string& streamType,
                     const cv::Mat& frame, uint64_t timestampUs,
                     int width, int height, const std::string& formatName);

private:
    int port_;
    std::string frontendDir_;
    std::unique_ptr<httplib::Server> svr_;
    std::atomic<bool> running_;

    std::thread httpThread_;
    std::thread encodeThread_;
    std::mutex encodeMutex_;
    std::condition_variable encodeCv_;

    // 单摄像头（向后兼容）
    StreamState colorState_;
    std::map<std::string, GripperWebState> gripperWebStates_;  // slot -> gripper state
    DeviceInfoState deviceInfoState_;
    PoseState poseState_;
    RecordingState recordingState_;
    ConvertState convertState_;
    std::map<std::string, UmiGripper*> umiGrippers_;  // slot -> gripper pointer (non-owning)
    std::map<std::string, ElectricGripper*> electricGrippers_;  // slot -> electric gripper (non-owning)
    std::map<std::string, ElectricGripperWebState> electricGripperWebStates_;  // slot -> state
    DeviceManager* deviceManager_ = nullptr;

    // 多摄像头流状态
    std::map<std::string, CameraStreamStates> cameraStates_;  // slot -> states

    std::string convertScriptPath_;
    std::string convertOutputDir_;
    std::thread convertThread_;

    // 保存任务队列：排队保存，第一个完成后第二个开始
    std::mutex finalizeQueueMutex_;
    std::condition_variable finalizeQueueCv_;
    std::list<FinalizeTask> finalizeTasks_;
    std::thread finalizeWorkerThread_;
    std::atomic<bool> finalizeWorkerRunning_{false};
    void finalizeWorker();
    void updateGlobalFinalizing();

    std::atomic<bool> colorEnabled_;
    std::atomic<bool> gripperEnabled_;

    // 多摄像头流开关
    std::mutex streamFlagMutex_;
    std::map<std::string, std::map<std::string, std::atomic<bool>>> streamFlags_;  // slot -> stream -> enabled

    void httpLoop();
    std::string buildStreamFpsJson();
    std::string buildCaptureFpsJson();
    std::string buildWarningsJson() const;
    void encodeLoop();
    void setupRoutes();
    void setupMultiCameraRoutes();

    bool startConversion(const std::string& sourceDir, const std::vector<std::string>& sessions,
                          const std::string& task, const std::string& outputDir = "",
                          const std::string& format = "lerobot");
    bool startRecording(const std::vector<std::string>& types,
                        const std::vector<std::string>& slots = {},
                        const std::vector<std::string>& streams = {},
                        const std::map<std::string, std::string>& slotMapping = {},
                        const std::string& saveMode = "strict");
    bool stopRecording();
    bool cancelFinalize(const std::string& sessionId);
    bool finalizeRecording(std::string sessionId,
                           std::string sessionPath,
                           std::string baseDir,
                           uint64_t startTime,
                           uint64_t stopTime,
                           std::map<std::string, RecordingState::SlotState> slots,
                           const std::function<bool()>& shouldCancel);
};
