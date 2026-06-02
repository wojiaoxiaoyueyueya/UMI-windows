#pragma once

#include <mutex>
#include <atomic>
#include <thread>
#include <queue>
#include <deque>
#include <condition_variable>
#include <fstream>
#include <opencv2/opencv.hpp>
#include <Eigen/Core>
#include <Eigen/Geometry>

struct SlamPose {
    bool valid;
    double timestamp;
    float tx, ty, tz;
    float qx, qy, qz, qw;
    float roll, pitch, yaw;
};

class SlamManager {
public:
    SlamManager();
    ~SlamManager();

    bool init(float fx, float fy, float cx, float cy, float depthScale = 0.001f);
    void feedRGBD(const cv::Mat& color, const cv::Mat& depth, uint64_t timestampUs);
    void feedIMU(float ax, float ay, float az,
                 float gx, float gy, float gz, uint64_t timestampUs);
    void getPose(SlamPose& out) const;
    bool isInitialized() const { return initialized_; }

    void startRecording(const std::string& csvPath);
    void stopRecording();
    uint64_t getRecordedFrameCount() const { return recordedFrameCount_; }

private:
    void workerLoop();
    static const int MAX_QUEUE_SIZE = 10;

    enum FrameType { RGBD, IMU_DATA };
    struct QueueItem {
        FrameType type;
        cv::Mat color, depth;
        float ax, ay, az, gx, gy, gz;
        uint64_t timestampUs;
    };

    struct ImuSample {
        float ax, ay, az;
        float gx, gy, gz;
        uint64_t timestampUs;
    };

    std::queue<QueueItem> queue_;
    mutable std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::atomic<bool> running_;
    std::thread workerThread_;

    bool initialized_;
    float fx_, fy_, cx_, cy_;
    float depthScale_;

    mutable std::mutex poseMutex_;
    SlamPose currentPose_;

    std::ofstream poseCsv_;
    std::atomic<bool> recording_;
    mutable std::mutex csvMutex_;
    std::atomic<uint64_t> recordedFrameCount_;

    std::deque<ImuSample> imuBuffer_;
    mutable std::mutex imuMutex_;

    Eigen::Vector3f gravityDir_;
    float gravityAlpha_;

    Eigen::Vector3f imuDeltaPos_;
    Eigen::Vector3f imuDeltaVel_;
    Eigen::Matrix3f imuDeltaRot_;
    uint64_t lastImuTimestamp_;
    bool hasImuData_;

    uint64_t lastVisualTimestamp_;

    std::vector<ImuSample> extractImuBetween(uint64_t fromTs, uint64_t toTs);
    Eigen::Matrix3f correctGravity(const Eigen::Matrix3f& R, const Eigen::Vector3f& gravity);
    static Eigen::Matrix3f skewExp(const Eigen::Vector3f& w, float dt);
};
