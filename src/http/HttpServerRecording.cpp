// HttpServerRecording.cpp - 数据录制与格式转换实现
// 这里集中处理采集落盘、统一时间戳、MP4 写入、会话收尾保存，以及 LeRobot/HDF5/RLDS 转换调度。

#include "HttpServer.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <windows.h>

#include "utils/WinFsUtils.hpp"
#include "utils/JsonHelper.hpp"

static uint64_t toSessionTimeUs(uint64_t timestampUs, uint64_t sessionStartUs) {
    return timestampUs >= sessionStartUs ? timestampUs - sessionStartUs : 0;
}

void HttpServer::recordGripper(const std::string& slot, float position, uint8_t button1, uint8_t button2, uint64_t timestampUs) {
    std::lock_guard<std::mutex> lock(recordingState_.mutex);
    if (!recordingState_.isRecording) return;

    // 判断当前槽位的夹爪数据是否被本次录制任务勾选，未勾选时直接跳过。
    if (!recordingState_.selectedStreams.empty()) {
        std::string key = slot + "-gripper";
        if (recordingState_.selectedStreams.find(key) == recordingState_.selectedStreams.end()) return;
    }

    std::string folder = RecordingState::positionToFolder(slot);
    auto it = recordingState_.slots.find(folder);
    if (it == recordingState_.slots.end()) return;

    auto& ss = it->second;
    // 第一次收到夹爪数据时再创建会话和槽位目录，避免没有数据时留下空目录。
    winfs::mkdirp(ss.slotDir);
    // 第一次写夹爪数据时创建 gripper_data 目录并打开 CSV，表头只写一次。
    if (!ss.gripperCsvFile.is_open()) {
        winfs::mkdirp(ss.slotDir + "/gripper_data");
        ss.gripperCsvFile.open(winfs::utf8ToAnsi(ss.slotDir + "/gripper_data/gripper.csv"));
        if (ss.gripperCsvFile.is_open())
            ss.gripperCsvFile << "timestamp_us,session_time_us,slot,position,button1,button2\n";
    }
    if (!ss.gripperCsvFile.is_open()) return;

    ss.gripperCsvFile << timestampUs
        << "," << toSessionTimeUs(timestampUs, recordingState_.startTime)
        << "," << slot
        << "," << std::fixed << std::setprecision(6) << position
        << "," << (int)button1 << "," << (int)button2 << "\n";
    ss.gripperCount++;
    if (position < ss.gripperMinPos) ss.gripperMinPos = position;
    if (position > ss.gripperMaxPos) ss.gripperMaxPos = position;
}

void HttpServer::recordElectricGripper(const std::string& slot, float positionDeg, float velocity, float current,
                                        float motorTemp, float mosTemp, uint8_t errorCode, uint64_t timestampUs) {
    std::lock_guard<std::mutex> lock(recordingState_.mutex);
    if (!recordingState_.isRecording) return;

    if (!recordingState_.selectedStreams.empty()) {
        std::string key = slot + "-gripper";
        if (recordingState_.selectedStreams.find(key) == recordingState_.selectedStreams.end()) return;
    }

    std::string folder = RecordingState::positionToFolder(slot);
    auto it = recordingState_.slots.find(folder);
    if (it == recordingState_.slots.end()) return;

    auto& ss = it->second;
    winfs::mkdirp(ss.slotDir);
    if (!ss.gripperCsvFile.is_open()) {
        winfs::mkdirp(ss.slotDir + "/gripper_data");
        ss.gripperCsvFile.open(winfs::utf8ToAnsi(ss.slotDir + "/gripper_data/gripper.csv"));
        if (ss.gripperCsvFile.is_open())
            ss.gripperCsvFile << "timestamp_us,session_time_us,slot,position_deg,velocity_rpm,current_a,"
                              << "motor_temp_c,mos_temp_c,error_code,motor_enabled\n";
    }
    if (!ss.gripperCsvFile.is_open()) return;

    ss.gripperCsvFile << timestampUs
        << "," << toSessionTimeUs(timestampUs, recordingState_.startTime)
        << "," << slot
        << "," << std::fixed << std::setprecision(4) << positionDeg
        << "," << std::fixed << std::setprecision(4) << velocity
        << "," << std::fixed << std::setprecision(4) << current
        << "," << std::fixed << std::setprecision(1) << motorTemp
        << "," << std::fixed << std::setprecision(1) << mosTemp
        << "," << (int)errorCode << "\n";
    ss.egCount++;
    if (positionDeg < ss.egMinPos) ss.egMinPos = positionDeg;
    if (positionDeg > ss.egMaxPos) ss.egMaxPos = positionDeg;
    if (velocity > ss.egMaxVel) ss.egMaxVel = velocity;
    if (current > ss.egMaxCur) ss.egMaxCur = current;
}


static cv::Mat makeRecordableFrame(const cv::Mat& frame) {
    if (frame.channels() == 3) return frame;

    cv::Mat bgr;
    if (frame.type() == CV_16UC1) {
        cv::Mat depth8;
        double minVal, maxVal;
        cv::minMaxLoc(frame, &minVal, &maxVal);
        if (maxVal > minVal)
            frame.convertTo(depth8, CV_8UC1, 255.0 / (maxVal - minVal), -255.0 * minVal / (maxVal - minVal));
        else
            depth8 = cv::Mat::zeros(frame.size(), CV_8UC1);
        cv::applyColorMap(depth8, bgr, cv::COLORMAP_JET);
    } else if (frame.channels() == 1) {
        cv::cvtColor(frame, bgr, cv::COLOR_GRAY2BGR);
    } else if (frame.channels() == 4) {
        cv::cvtColor(frame, bgr, cv::COLOR_BGRA2BGR);
    } else {
        frame.convertTo(bgr, CV_8UC3);
    }
    return bgr;
}

static bool openRecordingWriter(cv::VideoWriter& writer,
                                const std::string& path,
                                double fps,
                                const cv::Size& size) {
    int codec = cv::VideoWriter::fourcc('a', 'v', 'c', '1');
    if (writer.open(path, codec, fps, size, true)) return true;
    codec = cv::VideoWriter::fourcc('m', 'p', '4', 'v');
    if (writer.open(path, codec, fps, size, true)) return true;
    codec = cv::VideoWriter::fourcc('X', 'V', 'I', 'D');
    return writer.open(path, codec, fps, size, true);
}

static double clampRecordingFps(double fps) {
    if (fps < 5.0) return 5.0;
    if (fps > 120.0) return 120.0;
    return fps;
}

static const size_t RECORD_FPS_PROBE_FRAMES = 12;
static const uint64_t RECORD_FPS_PROBE_MAX_US = 500000;

static double estimateRecordingFps(const std::vector<uint64_t>& timestamps,
                                   double fallbackFps = 30.0,
                                   uint64_t sessionStartUs = 0,
                                   uint64_t sessionStopUs = 0) {
    if (timestamps.size() < 2) return clampRecordingFps(fallbackFps > 0.0 ? fallbackFps : 30.0);

    // 优先用“开始录制按钮到停止录制按钮”的会话时长估算 MP4 FPS，
    // 这样播放器显示的视频时长会和采集页显示的录制时长保持一致。
    if (sessionStopUs > sessionStartUs) {
        double sessionSpanSec = (double)(sessionStopUs - sessionStartUs) / 1000000.0;
        if (sessionSpanSec > 0.001) {
            return clampRecordingFps((double)(timestamps.size() - 1) / sessionSpanSec);
        }
    }

    // 缺少会话结束时间时，退回到首尾帧时间戳估算。
    if (timestamps.back() > timestamps.front()) {
        double frameSpanSec = (double)(timestamps.back() - timestamps.front()) / 1000000.0;
        if (frameSpanSec > 0.001) return clampRecordingFps((double)(timestamps.size() - 1) / frameSpanSec);
    }
    return clampRecordingFps(fallbackFps > 0.0 ? fallbackFps : 30.0);
}

static bool removeSessionDirAndHistory(const std::string& sessionPath,
                                       const std::string& baseDir,
                                       const std::string& sessionId) {
    bool ok = true;
    if (!sessionPath.empty() && winfs::dirExists(sessionPath)) {
        ok = winfs::removeDirRecursive(sessionPath);
    }

    // 取消保存后同步清理历史索引，避免页面历史里还出现已经删除的会话。
    std::string historyPath = baseDir + "/_history.txt";
    if (!sessionId.empty() && winfs::fileExists(historyPath)) {
        std::ifstream ifs(winfs::utf8ToAnsi(historyPath));
        std::vector<std::string> kept;
        std::string line;
        while (std::getline(ifs, line)) {
            if (line != sessionId && !line.empty()) kept.push_back(line);
        }
        ifs.close();

        std::ofstream ofs(winfs::utf8ToAnsi(historyPath), std::ios::trunc);
        if (ofs.is_open()) {
            for (const auto& sid : kept) ofs << sid << "\n";
        } else {
            ok = false;
        }
    }
    return ok;
}

void HttpServer::recordFrame(const std::string& slot, const std::string& streamType,
                              const cv::Mat& frame, uint64_t timestampUs,
                              int width, int height, const std::string& fmtName) {
    if (frame.empty()) return;

    bool fastModeSnapshot = false;
    {
        std::lock_guard<std::mutex> lock(recordingState_.mutex);
        if (!recordingState_.isRecording) return;
        fastModeSnapshot = recordingState_.fastSaveMode;
    }

    // ============================================================
    // 阶段 1（锁外）：统一转为 VideoWriter 可写的 BGR 帧。
    // 说明：为了让多路视频时长严格等于“开始录制到停止录制”的会话时长，
    // MP4 的最终 FPS 必须在停止录制后才能确定，因此默认使用严格同步缓存模式。
    // 快速保存模式下则会尽快打开 VideoWriter 并实时写入，停止更快但播放器时长可能轻微偏差。
    // ============================================================
    cv::Mat videoFrame = makeRecordableFrame(frame);
    std::vector<uint8_t> jpegBuf;
    if (!fastModeSnapshot) {
        std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 95};
        cv::imencode(".jpg", videoFrame, jpegBuf, params);
    }

    // ============================================================
    // 阶段 2（锁内）：只追加时间戳和压缩帧，尽量缩短相机回调被锁占用的时间。
    // ============================================================
    std::lock_guard<std::mutex> lock(recordingState_.mutex);
    if (!recordingState_.isRecording) return;

    std::string displayPos = slot;
    auto btdIt = recordingState_.backendToDisplay.find(slot);
    if (btdIt != recordingState_.backendToDisplay.end()) displayPos = btdIt->second;

    if (!recordingState_.selectedStreams.empty()) {
        std::string key = displayPos + "-" + streamType;
        if (recordingState_.selectedStreams.find(key) == recordingState_.selectedStreams.end()) return;
    }

    std::string folder = RecordingState::positionToFolder(displayPos);
    auto it = recordingState_.slots.find(folder);
    if (it == recordingState_.slots.end()) return;

    auto& ss = it->second;
    try {
        winfs::mkdirp(ss.slotDir);

        auto& si = ss.streamInfo[streamType];
        if (ss.frameCount[streamType] == 0) {
            std::string videoDir = ss.slotDir + "/" + streamType + "_video";
            winfs::mkdirp(videoDir);
            ss.timestampCsvs[streamType].open(winfs::utf8ToAnsi(videoDir + "/timestamps.csv"));
            if (ss.timestampCsvs[streamType].is_open())
                ss.timestampCsvs[streamType] << "frame_index,timestamp_us,session_time_us\n";

            si.format = fmtName;
            si.width = width;
            si.height = height;
            si.firstTimestamp = timestampUs;
            if (!ss.hasFirstVideoTimestamp) {
                ss.firstVideoDeviceTimestamp = timestampUs;
                ss.hasFirstVideoTimestamp = true;
            }
        }

        // 全量时间戳追加：CSV、metadata 和后续数据对齐都使用同一组时间戳。
        ss.allTimestamps[streamType].push_back(timestampUs);

        // CSV 时间戳写入
        auto csvIt = ss.timestampCsvs.find(streamType);
        if (csvIt != ss.timestampCsvs.end() && csvIt->second.is_open()) {
            csvIt->second << ss.frameCount[streamType]
                << "," << timestampUs
                << "," << toSessionTimeUs(timestampUs, recordingState_.startTime)
                << "\n";
        }

        if (recordingState_.fastSaveMode) {
            auto& writer = ss.videoWriters[streamType];
            auto& pendingFrames = ss.compressedFrames[streamType];
            auto& timestamps = ss.allTimestamps[streamType];

            if (writer.isOpened()) {
                writer.write(videoFrame);
            } else {
                // 快速模式只在开头缓存极少量探测帧，估出 FPS 后立即边录边写。
                std::vector<uint8_t> pendingJpeg;
                std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 95};
                cv::imencode(".jpg", videoFrame, pendingJpeg, params);
                pendingFrames.push_back(std::move(pendingJpeg));

                bool enoughFrames = pendingFrames.size() >= RECORD_FPS_PROBE_FRAMES;
                bool enoughSpan = timestamps.size() >= 2 &&
                    (timestamps.back() - timestamps.front()) >= RECORD_FPS_PROBE_MAX_US;
                if (enoughFrames || enoughSpan) {
                    double fps = estimateRecordingFps(timestamps, recordingState_.fps);
                    std::string videoDir = ss.slotDir + "/" + streamType + "_video";
                    winfs::mkdirp(videoDir);
                    std::string videoFile = videoDir + "/" + streamType + ".mp4";
                    if (openRecordingWriter(writer, winfs::utf8ToAnsi(videoFile), fps, videoFrame.size())) {
                        for (const auto& jpg : pendingFrames) {
                            cv::Mat decoded = cv::imdecode(jpg, cv::IMREAD_COLOR);
                            if (!decoded.empty()) writer.write(decoded);
                        }
                        pendingFrames.clear();
                        pendingFrames.shrink_to_fit();
                    }
                }
            }
        } else {
            // 严格同步模式：停止录制时按最终会话时长统一编码到 MP4。
            ss.compressedFrames[streamType].push_back(std::move(jpegBuf));
        }
    } catch (...) { return; }
    ss.frameCount[streamType]++;
}

std::string HttpServer::buildWarningsJson() const {
    std::string json;
    for (size_t i = 0; i < recordingState_.warnings.size(); i++) {
        if (i > 0) json += ",";
        json += "\"" + json::escape(recordingState_.warnings[i]) + "\"";
    }
    return json;
}

bool HttpServer::startRecording(const std::vector<std::string>& types,
                               const std::vector<std::string>& slots,
                               const std::vector<std::string>& streams,
                               const std::map<std::string, std::string>& slotMapping,
                               const std::string& saveMode) {
    // 保存队列独立运行，不需要在这里处理旧的 finalize 线程。

    std::lock_guard<std::mutex> lock(recordingState_.mutex);
    if (recordingState_.isRecording) return false;

    auto now = std::chrono::system_clock::now();
    auto timeT = std::chrono::system_clock::to_time_t(now);
    struct tm tmBuf;
    localtime_s(&tmBuf, &timeT);
    char sessionId[32];
    strftime(sessionId, sizeof(sessionId), "%Y%m%d_%H%M%S", &tmBuf);

    recordingState_.sessionId = sessionId;
    recordingState_.sessionPath = recordingState_.baseDir + "/" + sessionId;
    recordingState_.startTime = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    recordingState_.stopTime = 0;
    recordingState_.fps = 30.0;
    recordingState_.fastSaveMode = (saveMode == "fast");
    recordingState_.finalizing = false;
    recordingState_.slots.clear();
    recordingState_.selectedStreams.clear();
    recordingState_.warnings.clear();
    recordingState_.backendToDisplay.clear();
    for (const auto& s : streams) recordingState_.selectedStreams.insert(s);

    // 会话目录采用懒创建策略，只有真正收到视频或夹爪数据时才落盘。

    // 辅助判断指定显示位置和流类型是否被选中，键格式如 "left-color"。
    auto isStreamSelected = [&streams](const std::string& pos, const std::string& streamType) -> bool {
        if (streams.empty()) return true;
        std::string key = pos + "-" + streamType;
        for (const auto& s : streams) if (s == key) return true;
        return false;
    };

    // 根据前端传入的 display -> backend 映射，构建 backend -> display 的反向索引。
    // 反向索引用于在相机回调里把后端槽位还原为前端显示位置。
    for (const auto& kv : slotMapping) {
        recordingState_.backendToDisplay[kv.second] = kv.first;
    }
    // 没有显式映射的位置按同名处理，兼容旧版前端和默认槽位。
    for (const char* pos : {"left", "right", "head"}) {
        if (recordingState_.backendToDisplay.find(pos) == recordingState_.backendToDisplay.end()) {
            recordingState_.backendToDisplay[pos] = pos;
        }
    }

    // 遍历前端显示位置，为每个参与录制的位置建立独立的槽位状态。
    // 通过 slotMapping 找到显示位置对应的后端相机槽位。
    if (deviceManager_) {
        const char* displayPositions[] = {"left", "right", "head"};
        for (const char* displayPos : displayPositions) {
            // 查找当前显示位置绑定的后端槽位。
            std::string backendSlot;
            auto it = slotMapping.find(displayPos);
            if (it != slotMapping.end()) {
                backendSlot = it->second;
            } else {
                backendSlot = displayPos; // identity mapping
            }

            // 只要该显示位置下有任意视频流或夹爪流被勾选，就创建对应录制状态。
            bool anySelected = streams.empty();
            if (!anySelected) {
                for (const auto& s : streams) {
                    if (s.find(std::string(displayPos) + "-") == 0) { anySelected = true; break; }
                }
            }
            if (!anySelected) continue;

            // 根据后端槽位查找实际连接的相机设备。
            auto* slot = deviceManager_->getSlot(backendSlot);
            bool hasCamera = (slot && slot->connected);

            // 夹爪独立于相机映射，需要遍历所有夹爪槽位寻找与当前显示位置匹配的设备。
            // 手动夹爪通过 USB VID 固定左右手，不跟随相机 slotMapping 改变。
            bool hasGripper = false;
            auto* gslot = deviceManager_->getGripperSlot(displayPos);
            fprintf(stderr, "[RECORDING] Gripper check displayPos='%s' backendSlot='%s': "
                "gslot=%p connected=%d\n",
                displayPos, backendSlot.c_str(),
                (void*)gslot, gslot ? (int)gslot->connected : -1);
            if (gslot && gslot->connected) {
                hasGripper = true;
            } else {
                gslot = deviceManager_->getGripperSlot(backendSlot);
                fprintf(stderr, "[RECORDING] Gripper fallback backendSlot='%s': gslot=%p connected=%d\n",
                    backendSlot.c_str(), (void*)gslot, gslot ? (int)gslot->connected : -1);
                if (gslot && gslot->connected) {
                    hasGripper = true;
                } else {
                    for (auto& gkv : deviceManager_->getGripperSlotNames()) {
                        auto* gs = deviceManager_->getGripperSlot(gkv);
                        if (gs && gs->connected) {
                            hasGripper = true;
                            gslot = gs;
                            fprintf(stderr, "[RECORDING] Found connected gripper on slot '%s'\n", gkv.c_str());
                            break;
                        }
                    }
                }
            }

            if (!hasCamera && !hasGripper) {
                fprintf(stderr, "[RECORDING] 跳过 '%s': 无适配数据\n", displayPos);
                continue;
            }
            if (!hasCamera) {
                std::string warn = std::string(displayPos == "left" ? "左" : (displayPos == "right" ? "右" : "头")) + "手: 无视频数据，仅录制夹爪数据";
                recordingState_.warnings.push_back(warn);
                fprintf(stderr, "[RECORDING] %s\n", warn.c_str());
            }
            if (!hasGripper) {
                std::string warn = std::string(displayPos == "left" ? "左" : (displayPos == "right" ? "右" : "头")) + "手: 无夹爪数据，仅录制视频数据";
                recordingState_.warnings.push_back(warn);
                fprintf(stderr, "[RECORDING] %s\n", warn.c_str());
            }

            std::string folder = RecordingState::positionToFolder(displayPos);
            RecordingState::SlotState ss;
            ss.position = displayPos;
            ss.slotDir = recordingState_.sessionPath + "/" + folder;

            if (hasCamera && slot->camera) {
                ss.deviceType = slot->camera->getDeviceType();
            } else if (hasGripper) {
                ss.deviceType = gslot->gripperType;
            } else {
                ss.deviceType = "unknown";
            }

            bool isOrbbec = (ss.deviceType == "orbbec");

            fprintf(stderr, "[RECORDING] Display '%s' -> backend '%s' -> folder '%s' (type=%s)\n",
                    displayPos, backendSlot.c_str(), folder.c_str(), ss.deviceType.c_str());

            recordingState_.slots[folder] = std::move(ss);
        }
    }

    // 兜底逻辑：没有设备管理器或没有可用槽位时，仍创建默认槽位以兼容旧采集流程。
    if (recordingState_.slots.empty()) {
        RecordingState::SlotState ss;
        ss.position = "left";
        ss.deviceType = "unknown";
        std::string folder = RecordingState::positionToFolder("left");
        ss.slotDir = recordingState_.sessionPath + "/" + folder;
        recordingState_.slots[folder] = std::move(ss);
    }

    recordingState_.isRecording = true;
    return true;
}

bool HttpServer::stopRecording() {
    FinalizeTask task;
    {
        std::lock_guard<std::mutex> lock(recordingState_.mutex);
        if (!recordingState_.isRecording) return false;
        recordingState_.isRecording = false;
        recordingState_.finalizing = true;
        recordingState_.stopTime = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        task.sessionId = recordingState_.sessionId;
        task.sessionPath = recordingState_.sessionPath;
        task.baseDir = recordingState_.baseDir;
        task.startTime = recordingState_.startTime;
        task.stopTime = recordingState_.stopTime;
        task.fastSaveMode = recordingState_.fastSaveMode;
        task.slots = std::move(recordingState_.slots);
        task.status = FinalizeTask::PENDING;
    }

    {
        std::lock_guard<std::mutex> lock(finalizeQueueMutex_);
        finalizeTasks_.push_back(std::move(task));
    }
    finalizeQueueCv_.notify_one();

    return true;
}

bool HttpServer::finalizeRecording(std::string sessionId,
                                   std::string sessionPath,
                                   std::string baseDir,
                                   uint64_t startTime,
                                   uint64_t stopTime,
                                   std::map<std::string, RecordingState::SlotState> slots,
                                   const std::function<bool()>& shouldCancel) {
    auto cancelled = [&]() -> bool {
        return shouldCancel && shouldCancel();
    };

    // 停止录制后在后台完成收尾工作。
    // 严格同步模式下，录制期间只缓存 JPEG 帧和时间戳；
    // 这里按“最终会话时长”计算每路 MP4 FPS，保证不同视频播放器时长一致。
    for (auto& kv : slots) {
        if (cancelled()) return false;
        auto& ss = kv.second;

        // 释放所有已打开的 VideoWriter
        for (auto& writerKv : ss.videoWriters) {
            if (writerKv.second.isOpened()) writerKv.second.release();
        }

        // 将该槽位下每路缓存帧编码为 MP4；循环中持续检查取消状态，取消时快速中断。
        for (auto& compKv : ss.compressedFrames) {
            if (cancelled()) return false;
            const std::string& streamType = compKv.first;
            auto& frames = compKv.second;
            if (frames.empty()) continue;
            if (ss.videoWriters.count(streamType) && ss.videoWriters[streamType].isOpened()) continue;

            auto tsIt = ss.allTimestamps.find(streamType);
            if (tsIt == ss.allTimestamps.end() || tsIt->second.size() < 2) continue;

            double fps = estimateRecordingFps(tsIt->second, 30.0, startTime, stopTime);
            std::string videoDir = ss.slotDir + "/" + streamType + "_video";
            winfs::mkdirp(videoDir);
            std::string videoFile = videoDir + "/" + streamType + ".mp4";
            std::string path = winfs::utf8ToAnsi(videoFile);
            // 解码第一帧获取尺寸
            cv::Mat firstFrame = cv::imdecode(frames[0], cv::IMREAD_COLOR);
            if (firstFrame.empty()) continue;
            cv::VideoWriter writer;
            if (openRecordingWriter(writer, path, fps, firstFrame.size())) {
                writer.write(firstFrame);
                for (size_t i = 1; i < frames.size(); i++) {
                    if ((i % 8) == 0 && cancelled()) {
                        writer.release();
                        return false;
                    }
                    cv::Mat decoded = cv::imdecode(frames[i], cv::IMREAD_COLOR);
                    if (!decoded.empty()) writer.write(decoded);
                }
                writer.release();
            }
            frames.clear();
            frames.shrink_to_fit();
        }

        // 关闭 CSV 文件
        for (auto& csvKv : ss.timestampCsvs) {
            if (csvKv.second.is_open()) csvKv.second.close();
        }
        if (ss.gripperCsvFile.is_open()) ss.gripperCsvFile.close();
    }

    // 清理没有视频帧、没有手动夹爪数据、也没有电动夹爪数据的空槽位。
    bool anyData = false;
    for (auto it = slots.begin(); it != slots.end(); ) {
        auto& ss = it->second;
        uint64_t totalFrames = 0;
        for (auto& fcKv : ss.frameCount) totalFrames += fcKv.second;
        if (totalFrames == 0 && ss.gripperCount == 0 && ss.egCount == 0) {
            if (!ss.slotDir.empty()) {
                RemoveDirectoryA(winfs::utf8ToAnsi(ss.slotDir).c_str());
            }
            it = slots.erase(it);
        } else {
            anyData = true;
            ++it;
        }
    }

    if (!anyData) {
        removeSessionDirAndHistory(sessionPath, baseDir, sessionId);
        return true;
    }

    // 写入 metadata.json，记录会话时间基准、槽位信息、视频文件和夹爪统计。
    std::string metaPath = sessionPath + "/metadata.json";
    std::ofstream ofs(winfs::utf8ToAnsi(metaPath));
    if (ofs.is_open()) {
        ofs << "{\n  \"sessionId\": \"" << sessionId << "\",\n";
        ofs << "  \"startTimeUs\": " << startTime << ",\n";
        ofs << "  \"timeBase\": {\"timestamp_us\": \"system_epoch_us\", "
            << "\"session_time_us\": \"timestamp_us - startTimeUs\", "
            << "\"sync\": \"all recorded CSV files share session_time_us for alignment\"},\n";
        ofs << "  \"slots\": {\n";
        bool firstSlot = true;
        for (auto& kv : slots) {
            if (!firstSlot) ofs << ",\n";
            firstSlot = false;
            const std::string& folder = kv.first;
            auto& ss = kv.second;

            ofs << "    \"" << folder << "\": {\n";
            ofs << "      \"position\": \"" << ss.position << "\",\n";
            ofs << "      \"deviceType\": \"" << ss.deviceType << "\",\n";

            ofs << "      \"frameCount\": {";
            bool firstFC = true;
            for (auto& fcKv : ss.frameCount) {
                if (!firstFC) ofs << ", ";
                firstFC = false;
                ofs << "\"" << fcKv.first << "\": " << fcKv.second;
            }
            ofs << "},\n";

            ofs << "      \"gripperCount\": " << ss.gripperCount << ",\n";

            if (ss.hasFirstVideoTimestamp) {
                ofs << "      \"firstVideoDeviceTimestampUs\": " << ss.firstVideoDeviceTimestamp << ",\n";
                ofs << "      \"firstVideoSessionTimeUs\": " << toSessionTimeUs(ss.firstVideoDeviceTimestamp, startTime) << ",\n";
                ofs << "      \"recordingStartToFirstVideoOffsetUs\": "
                    << (int64_t)ss.firstVideoDeviceTimestamp - (int64_t)startTime << ",\n";
                ofs << "      \"deviceToSystemOffsetUs\": 0,\n";
            }

            ofs << "      \"videos\": {\n";
            bool firstVid = true;
            for (auto& siKv : ss.streamInfo) {
                if (siKv.second.width == 0) continue;
                if (!firstVid) ofs << ",\n";
                firstVid = false;
                ofs << "        \"" << siKv.first << "\": {\n";
                ofs << "          \"format\": \"" << siKv.second.format << "\",\n";
                ofs << "          \"width\": " << siKv.second.width << ",\n";
                ofs << "          \"height\": " << siKv.second.height << ",\n";
                uint64_t fc = ss.frameCount.count(siKv.first) ? ss.frameCount[siKv.first] : 0;
                ofs << "          \"frames\": " << fc << ",\n";
                ofs << "          \"firstTimestampUs\": " << siKv.second.firstTimestamp << ",\n";
                ofs << "          \"firstSessionTimeUs\": " << toSessionTimeUs(siKv.second.firstTimestamp, startTime) << ",\n";

                // 从全量时间戳计算精确 FPS 和时长
                auto tsIt = ss.allTimestamps.find(siKv.first);
                if (tsIt != ss.allTimestamps.end() && tsIt->second.size() >= 2) {
                    auto& tsVec = tsIt->second;
                    double actualFps = estimateRecordingFps(tsVec, 30.0, startTime, stopTime);
                    double durationSec = stopTime > startTime
                        ? (double)(stopTime - startTime) / 1000000.0
                        : (double)(tsVec.back() - tsVec.front()) / 1000000.0;
                    uint64_t lastTs = tsVec.back();
                    ofs << "          \"actualFps\": " << std::fixed << std::setprecision(2) << actualFps << ",\n";
                    ofs << "          \"durationSec\": " << std::fixed << std::setprecision(3) << durationSec << ",\n";
                    ofs << "          \"lastSessionTimeUs\": " << toSessionTimeUs(lastTs, startTime) << ",\n";
                }

                ofs << "          \"timestampFile\": \"" << siKv.first << "_video/timestamps.csv\",\n";
                ofs << "          \"file\": \"" << siKv.first << "_video/" << siKv.first << ".mp4\"\n";
                ofs << "        }";
            }
            ofs << "\n      },\n";

            ofs << "      \"gripper\": {\"file\": \"gripper_data/gripper.csv\", \"frames\": " << ss.gripperCount;
            if (ss.gripperCount > 0) {
                ofs << ", \"minPosition\": " << std::fixed << std::setprecision(6) << ss.gripperMinPos;
                ofs << ", \"maxPosition\": " << ss.gripperMaxPos;
            }
            if (ss.egCount > 0) {
                ofs << ", \"type\": \"electric\"";
                ofs << ", \"electricFrames\": " << ss.egCount;
                ofs << ", \"minPositionDeg\": " << std::fixed << std::setprecision(4) << ss.egMinPos;
                ofs << ", \"maxPositionDeg\": " << ss.egMaxPos;
                ofs << ", \"maxVelocityRpm\": " << std::fixed << std::setprecision(4) << ss.egMaxVel;
                ofs << ", \"maxCurrentA\": " << std::fixed << std::setprecision(4) << ss.egMaxCur;
            }
            ofs << "}\n";

            ofs << "    }";
        }
        ofs << "\n  }\n}\n";
    }

    std::string historyPath = baseDir + "/_history.txt";
    std::ofstream hofs(winfs::utf8ToAnsi(historyPath), std::ios::app);
    if (hofs.is_open()) hofs << sessionId << "\n";
    return true;
}

// ---- 保存任务队列 ----

void HttpServer::finalizeWorker() {
    while (true) {
        // 等待待处理的保存任务
        FinalizeTask* task = nullptr;
        {
            std::unique_lock<std::mutex> lock(finalizeQueueMutex_);
            while (finalizeWorkerRunning_) {
                for (auto& t : finalizeTasks_) {
                    if (t.status == FinalizeTask::PENDING) { task = &t; break; }
                }
                if (task) break;
                finalizeQueueCv_.wait_for(lock, std::chrono::milliseconds(500));
            }
            if (!finalizeWorkerRunning_ && !task) return;
            // 关闭时仍有待处理任务，继续处理
        }

        // 检查是否已取消
        bool isCancelled = false;
        std::string pathToDelete;
        std::string baseDirToDelete;
        std::string sessionIdToDelete;
        {
            std::lock_guard<std::mutex> lock(finalizeQueueMutex_);
            isCancelled = (task->status == FinalizeTask::CANCELLED);
            if (isCancelled) {
                pathToDelete = task->sessionPath;
                baseDirToDelete = task->baseDir;
                sessionIdToDelete = task->sessionId;
                finalizeTasks_.remove_if([task](const FinalizeTask& t) { return &t == task; });
            } else {
                task->status = FinalizeTask::RUNNING;
            }
        }
        if (isCancelled) {
            removeSessionDirAndHistory(pathToDelete, baseDirToDelete, sessionIdToDelete);
            updateGlobalFinalizing();
            continue;
        }

        // 移出数据进行 finalize（任务本身留在列表中跟踪状态）
        auto slots = std::move(task->slots);
        auto shouldCancel = [this, task]() -> bool {
            std::lock_guard<std::mutex> lock(finalizeQueueMutex_);
            return task->status == FinalizeTask::CANCELLED;
        };

        // 执行实际保存（耗时操作，不持锁）
        bool finalizeOk = finalizeRecording(task->sessionId, task->sessionPath,
                                            task->baseDir, task->startTime, task->stopTime,
                                            std::move(slots), shouldCancel);

        // 更新任务状态
        bool wasCancelled = false;
        {
            std::lock_guard<std::mutex> lock(finalizeQueueMutex_);
            wasCancelled = (task->status == FinalizeTask::CANCELLED) || !finalizeOk;
            if (!wasCancelled) {
                task->status = FinalizeTask::COMPLETED;
            }
        }

        // 如果在保存过程中被取消，删除已保存的数据
        if (wasCancelled) {
            removeSessionDirAndHistory(task->sessionPath, task->baseDir, task->sessionId);
            {
                std::lock_guard<std::mutex> lock(finalizeQueueMutex_);
                finalizeTasks_.remove_if([task](const FinalizeTask& t) { return &t == task; });
            }
        }

        updateGlobalFinalizing();
    }
}

void HttpServer::updateGlobalFinalizing() {
    bool anyActive = false;
    {
        std::lock_guard<std::mutex> lock(finalizeQueueMutex_);
        for (auto& t : finalizeTasks_) {
            if (t.status == FinalizeTask::PENDING || t.status == FinalizeTask::RUNNING) {
                anyActive = true;
                break;
            }
        }
    }
    if (!anyActive) {
        std::lock_guard<std::mutex> lock(recordingState_.mutex);
        recordingState_.finalizing = false;
    }
}

bool HttpServer::cancelFinalize(const std::string& sessionId) {
    std::string pathToDelete;
    std::string baseDirToDelete;
    std::string sessionIdToDelete;
    bool removeFromList = false;
    bool foundTask = false;
    {
        std::lock_guard<std::mutex> lock(finalizeQueueMutex_);
        for (auto it = finalizeTasks_.begin(); it != finalizeTasks_.end(); ++it) {
            if (it->sessionId == sessionId) {
                foundTask = true;
                pathToDelete = it->sessionPath;
                baseDirToDelete = it->baseDir;
                sessionIdToDelete = it->sessionId;
                if (it->status == FinalizeTask::COMPLETED) {
                    // 已完成的任务：直接从列表移除，随后删除本地文件
                    finalizeTasks_.erase(it);
                    removeFromList = true;
                } else if (it->status == FinalizeTask::PENDING || it->status == FinalizeTask::RUNNING) {
                    // 排队中或保存中：标记取消，worker 会在适当时机删除文件
                    it->status = FinalizeTask::CANCELLED;
                    finalizeQueueCv_.notify_one();
                }
                break;
            }
        }
    }

    // 如果任务已经从保存队列清理掉，但本地目录仍存在，则直接按当前采集根目录和 sessionId 删除。
    // 这对应前端点“取消”时任务刚好完成、状态不同步的情况，不再让用户等下一轮保存状态。
    if (!foundTask && !sessionId.empty()) {
        {
            std::lock_guard<std::mutex> lock(recordingState_.mutex);
            baseDirToDelete = recordingState_.baseDir;
        }
        pathToDelete = baseDirToDelete + "/" + sessionId;
        sessionIdToDelete = sessionId;
        removeFromList = true;
    }

    // 对已完成的任务，立即删除本地数据
    if (removeFromList) {
        removeSessionDirAndHistory(pathToDelete, baseDirToDelete, sessionIdToDelete);
        updateGlobalFinalizing();
    }
    return !pathToDelete.empty();
}

// ---- 数据转换 ----

bool HttpServer::startConversion(const std::string& sourceDir,
                                  const std::vector<std::string>& sessions,
                                  const std::string& task,
                                  const std::string& outputDir,
                                  const std::string& format) {
    if (sessions.empty()) return false;
    {
        std::lock_guard<std::mutex> lock(convertState_.mutex);
        if (convertState_.converting) return false;
        convertState_.converting = true;
        convertState_.totalSessions = (int)sessions.size();
        convertState_.completedSessions = 0;
        convertState_.error = "";
        convertState_.convertedSessions.clear();
        convertState_.skippedSessions.clear();
    }
    if (convertThread_.joinable()) convertThread_.join();

    convertThread_ = std::thread([this, sourceDir, sessions, task, outputDir, format]() {
        std::string projectRoot = recordingState_.baseDir;
        auto pos = projectRoot.rfind('/');
        if (pos == std::string::npos) pos = projectRoot.rfind('\\');
        if (pos != std::string::npos) projectRoot = projectRoot.substr(0, pos);

        std::string srcDir = recordingState_.baseDir;
        if (!sourceDir.empty()) {
            if (sourceDir[0] == '/' || sourceDir[0] == '\\' || sourceDir.size() >= 2 && sourceDir[1] == ':') {
                std::string r = winfs::resolvePath(sourceDir);
                if (winfs::dirExists(r)) srcDir = r;
            } else {
                std::string absPath = projectRoot + "/" + sourceDir;
                if (winfs::dirExists(absPath)) srcDir = absPath;
            }
        }

        std::string outDir = convertOutputDir_;
        if (!outputDir.empty()) {
            if (outputDir[0] == '/' || outputDir[0] == '\\' || outputDir.size() >= 2 && outputDir[1] == ':') {
                std::string r = winfs::resolvePath(outputDir);
                if (winfs::dirExists(r)) outDir = r;
            } else {
                outDir = projectRoot + "/" + outputDir;
            }
        }
        winfs::mkdirp(outDir);

        std::string script = convertScriptPath_;
        if (format == "hdf5") {
            auto p = script.rfind("convert_to_lerobot.py");
            if (p != std::string::npos) script.replace(p, 22, "convert_to_hdf5.py");
        } else if (format == "rlds") {
            auto p = script.rfind("convert_to_lerobot.py");
            if (p != std::string::npos) script.replace(p, 22, "convert_to_rlds.py");
        }

        for (int i = 0; i < (int)sessions.size(); i++) {
            {
                std::lock_guard<std::mutex> lk(convertState_.mutex);
                convertState_.currentSession = sessions[i];
                convertState_.currentStep = "Converting...";
            }
            std::string srcPath = srcDir + "/" + sessions[i];
            std::string outPath = outDir + "/" + sessions[i] + "_" + format;
            std::string progressPath = outDir + "/.convert_progress.json";

            // Windows: 使用 system() 调用 Python
            std::string cmd = "python \"" + script + "\" \"" + srcPath + "\" \"" + outPath + "\"";
            if (!task.empty()) cmd += " --task \"" + task + "\"";
            cmd += " --progress \"" + progressPath + "\"";

            int ret = system(cmd.c_str());

            if (ret == 2) {
                std::lock_guard<std::mutex> lk(convertState_.mutex);
                convertState_.skippedSessions.push_back(sessions[i]);
                convertState_.completedSessions = i + 1;
                continue;
            }
            std::lock_guard<std::mutex> lk(convertState_.mutex);
            if (ret != 0) {
                convertState_.error = sessions[i] + " failed (code " + std::to_string(ret) + ")";
                convertState_.converting = false;
                convertState_.currentStep = "Error";
                return;
            }
            convertState_.completedSessions = i + 1;
            convertState_.convertedSessions.push_back(sessions[i]);
        }
        {
            std::lock_guard<std::mutex> lk(convertState_.mutex);
            convertState_.converting = false;
            convertState_.currentSession = "";
            convertState_.currentStep = "Done";
        }
    });
    return true;
}
