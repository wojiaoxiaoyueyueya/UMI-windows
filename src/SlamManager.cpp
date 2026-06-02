#include "SlamManager.hpp"
#include <cstdio>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void quatToEuler(float qx, float qy, float qz, float qw,
                         float& roll, float& pitch, float& yaw) {
    float sinr = 2.0f * (qw * qx + qy * qz);
    float cosr = 1.0f - 2.0f * (qx * qx + qy * qy);
    roll = std::atan2(sinr, cosr) * 180.0f / (float)M_PI;

    float sinp = 2.0f * (qw * qy - qz * qx);
    if (std::fabs(sinp) >= 1.0f)
        pitch = std::copysign(90.0f, sinp);
    else
        pitch = std::asin(sinp) * 180.0f / (float)M_PI;

    float siny = 2.0f * (qw * qz + qx * qy);
    float cosy = 1.0f - 2.0f * (qy * qy + qz * qz);
    yaw = std::atan2(siny, cosy) * 180.0f / (float)M_PI;
}

// ---- 构造与析构 ----

SlamManager::SlamManager()
    : running_(false), initialized_(false),
      recording_(false), depthScale_(0.001f),
      fx_(0), fy_(0), cx_(0), cy_(0), recordedFrameCount_(0),
      gravityDir_(0.0f, -1.0f, 0.0f),
      gravityAlpha_(0.98f),
      imuDeltaPos_(Eigen::Vector3f::Zero()),
      imuDeltaVel_(Eigen::Vector3f::Zero()),
      imuDeltaRot_(Eigen::Matrix3f::Identity()),
      lastImuTimestamp_(0),
      hasImuData_(false),
      lastVisualTimestamp_(0) {
    memset(&currentPose_, 0, sizeof(currentPose_));
}

SlamManager::~SlamManager() {
    running_ = false;
    queueCv_.notify_all();
    if (workerThread_.joinable()) workerThread_.join();
    stopRecording();
}

// ---- 对外接口 ----

bool SlamManager::init(float fx, float fy, float cx, float cy, float depthScale) {
    fx_ = fx; fy_ = fy; cx_ = cx; cy_ = cy;
    depthScale_ = depthScale;
    initialized_ = true;
    running_ = true;
    workerThread_ = std::thread(&SlamManager::workerLoop, this);
    fprintf(stderr, "[SLAM] IMU-fused Visual Odometry started (fx=%.1f fy=%.1f cx=%.1f cy=%.1f)\n",
            fx_, fy_, cx_, cy_);
    return true;
}

void SlamManager::feedRGBD(const cv::Mat& color, const cv::Mat& depth,
                             uint64_t timestampUs) {
    QueueItem item;
    item.type = RGBD;
    item.color = color.clone();
    item.depth = depth.clone();
    item.timestampUs = timestampUs;
    std::lock_guard<std::mutex> lock(queueMutex_);
    if (queue_.size() >= MAX_QUEUE_SIZE) queue_.pop();
    queue_.push(std::move(item));
    queueCv_.notify_one();
}

void SlamManager::feedIMU(float ax, float ay, float az,
                            float gx, float gy, float gz, uint64_t timestampUs) {
    ImuSample s;
    s.ax = ax; s.ay = ay; s.az = az;
    s.gx = gx * (float)M_PI / 180.0f;
    s.gy = gy * (float)M_PI / 180.0f;
    s.gz = gz * (float)M_PI / 180.0f;
    s.timestampUs = timestampUs;
    {
        std::lock_guard<std::mutex> lock(imuMutex_);
        imuBuffer_.push_back(s);
        while (imuBuffer_.size() > 2000) imuBuffer_.pop_front();
    }
}

void SlamManager::getPose(SlamPose& out) const {
    std::lock_guard<std::mutex> lock(poseMutex_);
    out = currentPose_;
}

void SlamManager::startRecording(const std::string& csvPath) {
    std::lock_guard<std::mutex> lock(csvMutex_);
    if (poseCsv_.is_open()) poseCsv_.close();
    poseCsv_.open(csvPath);
    if (poseCsv_.is_open()) {
        poseCsv_ << "timestamp_us,x,y,z,qx,qy,qz,qw,roll,pitch,yaw\n";
        recording_ = true;
        recordedFrameCount_ = 0;
        fprintf(stderr, "[SLAM] Recording pose to %s\n", csvPath.c_str());
    }
}

void SlamManager::stopRecording() {
    recording_ = false;
    std::lock_guard<std::mutex> lock(csvMutex_);
    if (poseCsv_.is_open()) {
        poseCsv_.close();
        fprintf(stderr, "[SLAM] Pose recording stopped\n");
    }
}

// ---- 辅助函数 ----

static void recordPose(const SlamPose& pose, uint64_t ts, std::atomic<bool>& recording,
                       std::mutex& csvMutex, std::ofstream& poseCsv, std::atomic<uint64_t>& count) {
    if (recording) {
        std::lock_guard<std::mutex> lock(csvMutex);
        if (poseCsv.is_open()) {
            poseCsv << ts << "," << std::fixed << std::setprecision(6)
                << pose.tx << "," << pose.ty << "," << pose.tz << ","
                << pose.qx << "," << pose.qy << "," << pose.qz << "," << pose.qw << ","
                << pose.roll << "," << pose.pitch << "," << pose.yaw << "\n";
            count++;
        }
    }
}

static void detectAndBackproject(const cv::Mat& gray, const cv::Mat& depthM,
                                  cv::Ptr<cv::ORB>& detector,
                                  float fx, float fy, float cx, float cy,
                                  std::vector<cv::Point2f>& pts,
                                  std::vector<cv::Point3f>& pts3D) {
    std::vector<cv::KeyPoint> keypoints;
    detector->detect(gray, keypoints);
    pts.clear(); pts3D.clear();
    for (auto& kp : keypoints) {
        int ix = (int)kp.pt.x, iy = (int)kp.pt.y;
        if (ix < 0 || ix >= depthM.cols || iy < 0 || iy >= depthM.rows) continue;
        float d = depthM.at<float>(iy, ix);
        if (d < 0.1f || d > 10.0f) continue;
        pts.push_back(kp.pt);
        pts3D.push_back(cv::Point3f((kp.pt.x - cx) * d / fx,
                                     (kp.pt.y - cy) * d / fy, d));
    }
}

Eigen::Matrix3f SlamManager::skewExp(const Eigen::Vector3f& w, float dt) {
    Eigen::Vector3f angle = w * dt;
    float theta = angle.norm();
    if (theta < 1e-8f) return Eigen::Matrix3f::Identity();
    Eigen::Vector3f axis = angle / theta;
    float s = std::sin(theta), c = std::cos(theta);
    Eigen::Matrix3f K;
    K << 0, -axis.z(), axis.y(),
         axis.z(), 0, -axis.x(),
         -axis.y(), axis.x(), 0;
    return Eigen::Matrix3f::Identity() + s * K + (1.0f - c) * K * K;
}

std::vector<SlamManager::ImuSample> SlamManager::extractImuBetween(uint64_t fromTs, uint64_t toTs) {
    std::vector<ImuSample> result;
    std::lock_guard<std::mutex> lock(imuMutex_);
    for (auto& s : imuBuffer_) {
        if (s.timestampUs > fromTs && s.timestampUs <= toTs) {
            result.push_back(s);
        }
    }
    uint64_t cutoff = fromTs > 1000000 ? fromTs - 1000000 : 0;
    while (!imuBuffer_.empty() && imuBuffer_.front().timestampUs < cutoff) {
        imuBuffer_.pop_front();
    }
    return result;
}

Eigen::Matrix3f SlamManager::correctGravity(const Eigen::Matrix3f& R, const Eigen::Vector3f& gravity) {
    Eigen::Vector3f g = gravity.normalized();
    if (g.norm() < 0.5f) return R;

    Eigen::Vector3f up = R.col(1);
    Eigen::Vector3f target_up = -g;

    Eigen::Vector3f cross = up.cross(target_up);
    float dot = up.dot(target_up);
    float angle = std::atan2(cross.norm(), dot);

    if (angle < 0.001f) return R;

    Eigen::Vector3f axis = cross.normalized();
    float s = std::sin(angle), c = std::cos(angle);
    Eigen::Matrix3f K;
    K << 0, -axis.z(), axis.y(),
         axis.z(), 0, -axis.x(),
         -axis.y(), axis.x(), 0;
    Eigen::Matrix3f Rcorr = Eigen::Matrix3f::Identity() + s * K + (1.0f - c) * K * K;

    return Rcorr * R;
}

// ---- 后台处理循环：融合 IMU 与 RGBD 数据 ----

void SlamManager::workerLoop() {
    if (fx_ <= 0 || fy_ <= 0) {
        fprintf(stderr, "[SLAM] No camera intrinsics, running stub mode\n");
        while (running_) {
            QueueItem item;
            {
                std::unique_lock<std::mutex> lock(queueMutex_);
                queueCv_.wait_for(lock, std::chrono::milliseconds(100),
                    [this]() { return !queue_.empty() || !running_; });
                if (!running_ && queue_.empty()) break;
                if (queue_.empty()) continue;
                item = std::move(queue_.front());
                queue_.pop();
            }
            if (item.type == RGBD) {
                SlamPose pose;
                pose.valid = true;
                pose.timestamp = (double)item.timestampUs / 1e6;
                pose.qw = 1;
                { std::lock_guard<std::mutex> lock(poseMutex_); currentPose_ = pose; }
            }
        }
        return;
    }

    cv::Mat K = (cv::Mat_<double>(3,3) << fx_, 0, cx_, 0, fy_, cy_, 0, 0, 1);
    cv::Mat distCoeffs = cv::Mat::zeros(4, 1, CV_64F);
    cv::Ptr<cv::ORB> detector = cv::ORB::create(1500);

    cv::Mat prevGray;
    std::vector<cv::Point2f> prevPts;
    std::vector<cv::Point3f> prevPts3D;

    Eigen::Matrix4f Tcw = Eigen::Matrix4f::Identity();

    bool firstFrame = true;
    int frameCount = 0, trackOk = 0, failStreak = 0;
    int imuOnlyCount = 0;

    while (running_) {
        QueueItem item;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCv_.wait_for(lock, std::chrono::milliseconds(100),
                [this]() { return !queue_.empty() || !running_; });
            if (!running_ && queue_.empty()) break;
            if (queue_.empty()) continue;
            item = std::move(queue_.front());
            queue_.pop();
        }

        if (item.type != RGBD) continue;
        frameCount++;

        uint64_t curTs = item.timestampUs;

        cv::Mat gray;
        if (item.color.channels() == 3)
            cv::cvtColor(item.color, gray, cv::COLOR_BGR2GRAY);
        else
            gray = item.color.clone();

        cv::Mat depthM;
        item.depth.convertTo(depthM, CV_32F, depthScale_);

        // IMU 预积分：在相邻图像帧之间累计惯性测量，改善位姿连续性。
        Eigen::Matrix3f imuRelRot = Eigen::Matrix3f::Identity();
        Eigen::Vector3f imuRelTrans = Eigen::Vector3f::Zero();
        bool hasImuPrediction = false;

        if (hasImuData_ && lastVisualTimestamp_ > 0) {
            auto imuSamples = extractImuBetween(lastVisualTimestamp_, curTs);
            if (!imuSamples.empty()) {
                Eigen::Matrix3f dR = Eigen::Matrix3f::Identity();
                Eigen::Vector3f dV = Eigen::Vector3f::Zero();
                Eigen::Vector3f dP = Eigen::Vector3f::Zero();
                uint64_t prevTs = imuSamples.front().timestampUs;

                for (auto& s : imuSamples) {
                    float dt = (float)(s.timestampUs - prevTs) / 1e6f;
                    if (dt <= 0 || dt > 0.1f) { prevTs = s.timestampUs; continue; }
                    dR = skewExp(Eigen::Vector3f(s.gx, s.gy, s.gz), dt) * dR;
                    Eigen::Vector3f accel(s.ax, s.ay, s.az);
                    Eigen::Vector3f gravComp = 9.81f * gravityDir_;
                    Eigen::Vector3f linAccel = accel - gravComp;
                    dV += linAccel * dt;
                    dP += dV * dt;
                    prevTs = s.timestampUs;
                }

                imuRelRot = dR;
                imuRelTrans = dP;

                for (auto& s : imuSamples) {
                    Eigen::Vector3f a(s.ax, s.ay, s.az);
                    float aNorm = a.norm();
                    if (aNorm > 5.0f && aNorm < 15.0f) {
                        Eigen::Vector3f aDir = a / aNorm;
                        gravityDir_ = gravityAlpha_ * gravityDir_ + (1.0f - gravityAlpha_) * aDir;
                        gravityDir_.normalize();
                    }
                }
                hasImuPrediction = true;
            }
        }

        lastVisualTimestamp_ = curTs;

        if (prevPts.empty() && !firstFrame) {
            detectAndBackproject(gray, depthM, detector, fx_, fy_, cx_, cy_,
                                 prevPts, prevPts3D);
            prevGray = gray.clone();
            continue;
        }

        if (firstFrame) {
            detectAndBackproject(gray, depthM, detector, fx_, fy_, cx_, cy_,
                                 prevPts, prevPts3D);
            prevGray = gray.clone();
            firstFrame = false;
            SlamPose pose;
            pose.valid = true;
            pose.timestamp = (double)item.timestampUs / 1e6;
            pose.qw = 1;
            { std::lock_guard<std::mutex> lock(poseMutex_); currentPose_ = pose; }
            fprintf(stderr, "[SLAM] Frame %d: VO init with %zu features, IMU=%s\n",
                    frameCount, prevPts.size(), hasImuData_ ? "yes" : "no");
            continue;
        }

        // 光流跟踪：用相邻彩色帧估计短时间运动趋势。
        std::vector<cv::Point2f> nextPts;
        std::vector<uchar> status;
        std::vector<float> err;
        cv::calcOpticalFlowPyrLK(prevGray, gray, prevPts, nextPts, status, err,
                                   cv::Size(21, 21), 3);

        std::vector<cv::Point3f> objPts;
        std::vector<cv::Point2f> imgPts;
        for (size_t i = 0; i < status.size(); i++) {
            if (!status[i]) continue;
            int x = (int)nextPts[i].x, y = (int)nextPts[i].y;
            if (x < 0 || x >= depthM.cols || y < 0 || y >= depthM.rows) continue;
            objPts.push_back(prevPts3D[i]);
            imgPts.push_back(nextPts[i]);
        }

        bool tracked = false;

        if ((int)objPts.size() >= 30) {
            cv::Mat rvec, tvec;
            bool useInitialGuess = false;
            cv::Mat initialRvec, initialTvec;
            if (hasImuPrediction) {
                cv::Mat relRotCv = (cv::Mat_<double>(3,3) <<
                    imuRelRot(0,0), imuRelRot(0,1), imuRelRot(0,2),
                    imuRelRot(1,0), imuRelRot(1,1), imuRelRot(1,2),
                    imuRelRot(2,0), imuRelRot(2,1), imuRelRot(2,2));
                cv::Rodrigues(relRotCv, initialRvec);
                initialTvec = (cv::Mat_<double>(3,1) <<
                    imuRelTrans(0), imuRelTrans(1), imuRelTrans(2));
                useInitialGuess = true;
            }

            bool ok = cv::solvePnPRansac(objPts, imgPts, K, distCoeffs, rvec, tvec,
                                          useInitialGuess, 100, 4.0f, 0.99);
            if (!ok && useInitialGuess) {
                ok = cv::solvePnPRansac(objPts, imgPts, K, distCoeffs, rvec, tvec,
                                          false, 100, 4.0, 0.99);
            }

            if (ok) {
                cv::Mat R;
                cv::Rodrigues(rvec, R);
                bool nan = false;
                for (int i = 0; i < 9; i++)
                    if (std::isnan(R.at<double>(i / 3, i % 3))) nan = true;
                for (int i = 0; i < 3; i++)
                    if (std::isnan(tvec.at<double>(i))) nan = true;

                if (!nan) {
                    Eigen::Matrix4f Trel_vo = Eigen::Matrix4f::Identity();
                    for (int i = 0; i < 3; i++)
                        for (int j = 0; j < 3; j++)
                            Trel_vo(i,j) = (float)R.at<double>(i,j);
                    Trel_vo(0,3) = (float)tvec.at<double>(0);
                    Trel_vo(1,3) = (float)tvec.at<double>(1);
                    Trel_vo(2,3) = (float)tvec.at<double>(2);

                    Eigen::Vector3f voTrans = Trel_vo.block<3,1>(0,3);
                    bool voConsistent = true;
                    if (hasImuPrediction) {
                        Eigen::Matrix3f voRot = Trel_vo.block<3,3>(0,0);
                        Eigen::Matrix3f Rdiff = voRot * imuRelRot.transpose();
                        float angle = std::acos(std::min(1.0f, (Rdiff.trace() - 1.0f) / 2.0f));
                        angle = std::fabs(angle);
                        if (angle > 0.26f) voConsistent = false;
                        float transDiff = (voTrans - imuRelTrans).norm();
                        if (transDiff > 0.3f && voTrans.norm() > 0.3f) voConsistent = false;
                    }
                    if (voTrans.norm() >= 0.5f) voConsistent = false;

                    if (voConsistent) {
                        Tcw = Trel_vo * Tcw;
                        Eigen::Matrix4f Twc = Tcw.inverse();
                        Eigen::Matrix3f Rwc = Twc.block<3,3>(0,0);

                        if (gravityDir_.norm() > 0.5f) {
                            Rwc = correctGravity(Rwc, gravityDir_);
                            Twc.block<3,3>(0,0) = Rwc;
                            Tcw = Twc.inverse();
                        }

                        Eigen::Vector3f twc = Twc.block<3,1>(0,3);
                        Eigen::Quaternionf q(Rwc);

                        SlamPose pose;
                        pose.valid = true;
                        pose.timestamp = (double)item.timestampUs / 1e6;
                        pose.tx = twc.x(); pose.ty = twc.y(); pose.tz = twc.z();
                        pose.qx = q.x(); pose.qy = q.y(); pose.qz = q.z(); pose.qw = q.w();
                        quatToEuler(pose.qx, pose.qy, pose.qz, pose.qw,
                                     pose.roll, pose.pitch, pose.yaw);
                        { std::lock_guard<std::mutex> lock(poseMutex_); currentPose_ = pose; }

                        trackOk++;
                        tracked = true;
                        imuOnlyCount = 0;

                        std::vector<cv::Point3f> new3D;
                        std::vector<cv::Point2f> new2D;
                        for (size_t i = 0; i < imgPts.size(); i++) {
                            int px = (int)imgPts[i].x, py = (int)imgPts[i].y;
                            if (px < 0 || px >= depthM.cols || py < 0 || py >= depthM.rows) continue;
                            float d = depthM.at<float>(py, px);
                            if (d < 0.1f || d > 10.0f) continue;
                            new3D.push_back(cv::Point3f(
                                (imgPts[i].x - cx_) * d / fx_,
                                (imgPts[i].y - cy_) * d / fy_, d));
                            new2D.push_back(imgPts[i]);
                        }
                        if ((int)new3D.size() >= 20) {
                            prevPts = new2D;
                            prevPts3D = new3D;
                        } else {
                            tracked = false;
                        }

                        recordPose(pose, item.timestampUs, recording_, csvMutex_, poseCsv_, recordedFrameCount_);
                    }
                }
            }
        }

        // IMU 兜底：视觉结果不可用时，用惯性数据维持基本姿态估计。
        if (!tracked && hasImuPrediction && imuOnlyCount < 30) {
            Eigen::Matrix4f Trel_imu = Eigen::Matrix4f::Identity();
            Trel_imu.block<3,3>(0,0) = imuRelRot;
            Trel_imu.block<3,1>(0,3) = imuRelTrans;

            Tcw = Trel_imu * Tcw;
            Eigen::Matrix4f Twc = Tcw.inverse();
            Eigen::Matrix3f Rwc = Twc.block<3,3>(0,0);

            if (gravityDir_.norm() > 0.5f) {
                Rwc = correctGravity(Rwc, gravityDir_);
                Twc.block<3,3>(0,0) = Rwc;
                Tcw = Twc.inverse();
            }

            Eigen::Vector3f twc = Twc.block<3,1>(0,3);
            Eigen::Quaternionf q(Rwc);

            SlamPose pose;
            pose.valid = true;
            pose.timestamp = (double)item.timestampUs / 1e6;
            pose.tx = twc.x(); pose.ty = twc.y(); pose.tz = twc.z();
            pose.qx = q.x(); pose.qy = q.y(); pose.qz = q.z(); pose.qw = q.w();
            quatToEuler(pose.qx, pose.qy, pose.qz, pose.qw,
                         pose.roll, pose.pitch, pose.yaw);
            { std::lock_guard<std::mutex> lock(poseMutex_); currentPose_ = pose; }

            tracked = true;
            imuOnlyCount++;
            recordPose(pose, item.timestampUs, recording_, csvMutex_, poseCsv_, recordedFrameCount_);
        }

        {
            std::lock_guard<std::mutex> lock(imuMutex_);
            hasImuData_ = !imuBuffer_.empty();
        }

        failStreak = tracked ? 0 : failStreak + 1;
        if (!tracked || failStreak > 5) {
            detectAndBackproject(gray, depthM, detector, fx_, fy_, cx_, cy_,
                                 prevPts, prevPts3D);
            if (tracked) failStreak = 0;
        }

        prevGray = gray.clone();
    }
}
