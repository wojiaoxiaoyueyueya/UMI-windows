// main.cpp - 多摄像头 + 夹爪数据采集主程序（Windows 版）
// 支持：海康威视工业相机、Orbbec Gemini 305 深度相机
// 夹爪：手动(UMI)、电动(预留) 各最多两个，左右分组
// 架构：DeviceManager 检测设备 → 分配左右槽位 → 多线程采集

#include "Config.hpp"
#include "HttpServer.hpp"
#include "UmiGripper.hpp"
#include "IGripper.hpp"
#include "DeviceManager.hpp"
#include "ICamera.hpp"
#include "HikCameraAdapter.hpp"
#include "OrbbecCamera.hpp"
#include "SlamManager.hpp"

#include <thread>
#include <chrono>
#include <iostream>
#include <cstdio>
#include <map>
#include <winsock2.h>
#include <windows.h>

static std::string getExecutableDir() {
    char path[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (len > 0) {
        char* lastSlash = strrchr(path, '\\');
        if (!lastSlash) lastSlash = strrchr(path, '/');
        if (lastSlash) *lastSlash = '\0';
        return std::string(path);
    }
    return ".";
}

static volatile LONG g_running = 1;
static volatile LONG g_stopCount = 0;

// 崩溃日志：捕获段错误并写入文件，方便定位
static std::string g_exeDir;
static LONG WINAPI crashHandler(EXCEPTION_POINTERS* info) {
    std::string logPath = g_exeDir + "/crash.log";
    FILE* f = fopen(logPath.c_str(), "a");
    if (f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] CRASH\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        fprintf(f, "  Exception code: 0x%08lX\n", info->ExceptionRecord->ExceptionCode);
        fprintf(f, "  Address: 0x%p\n", info->ExceptionRecord->ExceptionAddress);

        // 找出崩溃地址所在的 DLL
        HMODULE hModule = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                          (LPCSTR)info->ExceptionRecord->ExceptionAddress,
                          &hModule);
        if (hModule) {
            char moduleName[MAX_PATH];
            GetModuleFileNameA(hModule, moduleName, MAX_PATH);
            fprintf(f, "  Module: %s (base=0x%p, offset=0x%tx)\n",
                    moduleName, hModule,
                    (ptrdiff_t)info->ExceptionRecord->ExceptionAddress - (ptrdiff_t)hModule);
        } else {
            fprintf(f, "  Module: unknown\n");
        }

        for (DWORD i = 0; i < info->ExceptionRecord->NumberParameters && i < 2; i++)
            fprintf(f, "  Param[%lu]: 0x%p\n", i, (void*)info->ExceptionRecord->ExceptionInformation[i]);
        fclose(f);
    }
    fprintf(stderr, "\n=== CRASH: 0x%08lX at %p (see crash.log) ===\n",
            info->ExceptionRecord->ExceptionCode, info->ExceptionRecord->ExceptionAddress);
    Sleep(3000);
    return EXCEPTION_EXECUTE_HANDLER;
}

static BOOL WINAPI consoleHandler(DWORD signal) {
    if (InterlockedIncrement(&g_stopCount) >= 2) ExitProcess(0);
    InterlockedExchange(&g_running, 0);

    // 关闭终端窗口 / 注销 / 关机时交给系统默认处理终止进程
    // 返回 FALSE → 系统默认处理器会安全地终止进程
    // 返回 TRUE  → 进程会在后台继续运行（视频流不断）
    if (signal == CTRL_CLOSE_EVENT ||
        signal == CTRL_LOGOFF_EVENT ||
        signal == CTRL_SHUTDOWN_EVENT) {
        return FALSE;
    }
    return TRUE;
}

int main(int argc, char* argv[]) {
    try {
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
    SetConsoleCtrlHandler(consoleHandler, TRUE);

    // ---- 0. 加载配置 ----
    std::string exeDir = getExecutableDir();
    g_exeDir = exeDir;
    SetUnhandledExceptionFilter(crashHandler);
    Config cfg = Config::load(exeDir);

    // ---- 1. 初始化设备管理器，检测所有设备 ----
    DeviceManager deviceMgr(cfg);
    deviceMgr.detectAll();

    // ---- 2. 初始化 HTTP 服务器 ----
    std::string frontendDir = exeDir + "/../" + cfg.paths.frontendDir;
    HttpServer server(cfg, frontendDir);
    server.setDeviceManager(&deviceMgr);

    // 为每个已连接的设备设置服务器设备信息
    for (auto& slotName : deviceMgr.getSlotNames()) {
        auto* slot = deviceMgr.getSlot(slotName);
        if (slot && slot->connected && slot->camera) {
            server.updateDeviceInfo(slot->camera->getDeviceName(), 0, 0,
                                    slot->camera->getSerialNumber(), "N/A");
            server.setStreamActive(slotName, "color", false);
            if (slot->camera->hasDepthStream()) {
                server.setStreamActive(slotName, "depth", false);
            }
            if (slot->camera->hasIRStream()) {
                server.setStreamActive(slotName, "ir-left", false);
                server.setStreamActive(slotName, "ir-right", false);
            }
            // 向后兼容
            if (slotName == "left") {
                server.setStreamActive("color", false);
            }
        }
    }

    server.start();

    std::vector<std::thread> cameraThreads;
    std::map<std::string, std::unique_ptr<SlamManager>> slamManagers;
    std::map<std::string, ICamera*> activatedCameraPtrs;
    std::map<std::string, IGripper*> activatedGripperPtrs;

    auto activateGripperSlot = [&](const std::string& slotName) {
        auto* gslot = deviceMgr.getGripperSlot(slotName);
        IGripper* current = (gslot && gslot->connected && gslot->gripper) ? gslot->gripper.get() : nullptr;
        auto it = activatedGripperPtrs.find(slotName);
        if (!current) {
            if (it != activatedGripperPtrs.end()) {
                server.setUmiGripper(slotName, nullptr);
                server.setElectricGripper(slotName, nullptr);
                activatedGripperPtrs.erase(it);
                fprintf(stderr, "[热插拔] 清除 %s 夹爪引用\n", slotName.c_str());
            }
            return;
        }
        if (it != activatedGripperPtrs.end() && it->second == current) return;

        server.setUmiGripper(slotName, nullptr);
        server.setElectricGripper(slotName, nullptr);

        if (gslot->gripperType == "manual") {
            auto* umiPtr = dynamic_cast<UmiGripper*>(current);
            if (umiPtr) {
                server.setUmiGripper(slotName, umiPtr);
                fprintf(stderr, "[热插拔] 已激活手动夹爪 (%s槽)\n", slotName.c_str());
            }
        } else if (gslot->gripperType == "electric") {
            auto* ePtr = dynamic_cast<ElectricGripper*>(current);
            if (ePtr) {
                server.setElectricGripper(slotName, ePtr);
                ePtr->queryPosition();
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                ePtr->queryCurrent();
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                fprintf(stderr, "[热插拔] 已激活电动夹爪 (%s槽)\n", slotName.c_str());
            }
        }

        activatedGripperPtrs[slotName] = current;
    };

    auto activateCameraSlot = [&](const std::string& slotName) {
        auto* slot = deviceMgr.getSlot(slotName);
        ICamera* cam = (slot && slot->connected && slot->camera) ? slot->camera.get() : nullptr;
        auto it = activatedCameraPtrs.find(slotName);
        if (!cam) {
            if (it != activatedCameraPtrs.end()) {
                activatedCameraPtrs.erase(it);
                fprintf(stderr, "[热插拔] %s 槽相机已移除\n", slotName.c_str());
            }
            return;
        }
        if (it != activatedCameraPtrs.end() && it->second == cam) return;

        server.updateDeviceInfo(cam->getDeviceName(), 0, 0, cam->getSerialNumber(), "N/A");
        server.setStreamActive(slotName, "color", false);
        if (cam->hasDepthStream()) server.setStreamActive(slotName, "depth", false);
        if (cam->hasIRStream()) {
            server.setStreamActive(slotName, "ir-left", false);
            server.setStreamActive(slotName, "ir-right", false);
        }
        if (slotName == "left") server.setStreamActive("color", false);

        int frameSkipMs = cfg.stream.frameSkipMs;
        if (cam->getDeviceType() == "hikvision") {
            cameraThreads.emplace_back([cam, &server, slotName, frameSkipMs]() {
                uint64_t lastTime = 0;
                auto lastFrameTime = std::chrono::steady_clock::now();
                std::string camSerial = cam->getSerialNumber();
                int reconnectFailures = 0;
                fprintf(stderr, "[%s] 海康相机热插拔线程已启动 (SN: %s)\n", slotName.c_str(), camSerial.c_str());

                while (g_running) {
                    cv::Mat frame = cam->readColor();
                    if (frame.empty()) {
                        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now() - lastFrameTime).count();
                        if (elapsed >= 5) {
                            cam->close();
                            std::this_thread::sleep_for(std::chrono::milliseconds(500));
                            if (!cam->open(0, camSerial)) {
                                reconnectFailures++;
                                if (reconnectFailures >= 3) {
                                    fprintf(stderr, "[%s] Hikvision camera reconnect failed repeatedly, thread exits (SN: %s)\n",
                                            slotName.c_str(), camSerial.c_str());
                                    break;
                                }
                                std::this_thread::sleep_for(std::chrono::seconds(5));
                                continue;
                            }
                            reconnectFailures = 0;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                        continue;
                    }

                    lastFrameTime = std::chrono::steady_clock::now();
                    reconnectFailures = 0;
                    server.tickCaptureFrame(slotName, "color");
                    if (server.isRecording()) {
                        uint64_t ts = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
                        server.recordFrame(slotName, "color", frame, ts, frame.cols, frame.rows, "BGR");
                    }
                    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                    if (server.isStreamActive(slotName, "color") || server.isStreamActive("color")) {
                        if (now - lastTime >= frameSkipMs) {
                            lastTime = now;
                            server.updateColorFrame(slotName, frame);
                        }
                    }
                }
            });
        } else if (cam->getDeviceType() == "orbbec") {
            if (cfg.slam.enabled && cam->hasDepthStream()) {
                float fx, fy, cx, cy;
                if (cam->getIntrinsics(fx, fy, cx, cy)) {
                    auto slam = std::make_unique<SlamManager>();
                    slam->init(fx, fy, cx, cy, (float)cfg.slam.depthScale);
                    slamManagers[slotName] = std::move(slam);
                }
            }

            cam->setColorCallback([&server, slotName, frameSkipMs, &slamManagers](const cv::Mat& frame, uint64_t) {
                static std::map<std::string, uint64_t> lastTimeMap;
                uint64_t& lastTime = lastTimeMap[slotName];
                server.tickCaptureFrame(slotName, "color");
                auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                if (server.isStreamActive(slotName, "color") || server.isStreamActive("color")) {
                    if (now - lastTime >= frameSkipMs) {
                        lastTime = now;
                        server.updateColorFrame(slotName, frame);
                    }
                }
                if (server.isRecording()) {
                    uint64_t ts = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    server.recordFrame(slotName, "color", frame, ts, frame.cols, frame.rows, "BGR");
                }
                auto slamIt = slamManagers.find(slotName);
                if (slamIt != slamManagers.end() && slamIt->second && slamIt->second->isInitialized()) {
                    SlamPose pose;
                    slamIt->second->getPose(pose);
                    if (pose.valid) {
                        server.updatePoseData(pose.tx, pose.ty, pose.tz,
                                              pose.qx, pose.qy, pose.qz, pose.qw,
                                              pose.roll, pose.pitch, pose.yaw,
                                              (uint64_t)(pose.timestamp * 1e6));
                    }
                }
            });
            cam->setDepthCallback([&server, slotName, frameSkipMs](const cv::Mat& visualization, const cv::Mat& rawDepth, uint64_t) {
                static std::map<std::string, uint64_t> lastTimeMap;
                uint64_t& lastTime = lastTimeMap[slotName];
                server.tickCaptureFrame(slotName, "depth");
                auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                if (server.isStreamActive(slotName, "depth") && now - lastTime >= frameSkipMs) {
                    lastTime = now;
                    server.updateDepthFrame(slotName, visualization);
                }
                if (server.isRecording() && !rawDepth.empty()) {
                    uint64_t ts = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    server.recordFrame(slotName, "depth", rawDepth, ts, rawDepth.cols, rawDepth.rows, "Y16");
                }
            });
            cam->setIRLeftCallback([&server, slotName, frameSkipMs](const cv::Mat& irFrame, uint64_t) {
                static std::map<std::string, uint64_t> lastTimeMap;
                uint64_t& lastTime = lastTimeMap[slotName + "_irl"];
                server.tickCaptureFrame(slotName, "ir-left");
                auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                if (server.isStreamActive(slotName, "ir-left") && now - lastTime >= frameSkipMs) {
                    lastTime = now;
                    server.updateIRLeftFrame(slotName, irFrame);
                }
                if (server.isRecording() && !irFrame.empty()) {
                    uint64_t ts = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    server.recordFrame(slotName, "ir-left", irFrame, ts, irFrame.cols, irFrame.rows, "Y8");
                }
            });
            cam->setIRRightCallback([&server, slotName, frameSkipMs](const cv::Mat& irFrame, uint64_t) {
                static std::map<std::string, uint64_t> lastTimeMap;
                uint64_t& lastTime = lastTimeMap[slotName + "_irr"];
                server.tickCaptureFrame(slotName, "ir-right");
                auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                if (server.isStreamActive(slotName, "ir-right") && now - lastTime >= frameSkipMs) {
                    lastTime = now;
                    server.updateIRRightFrame(slotName, irFrame);
                }
                if (server.isRecording() && !irFrame.empty()) {
                    uint64_t ts = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    server.recordFrame(slotName, "ir-right", irFrame, ts, irFrame.cols, irFrame.rows, "Y8");
                }
            });
            cam->setPointCloudCallback([&server, slotName](const std::vector<float>& points, int width, int height, uint64_t) {
                if (!points.empty()) server.tickPointCloudFrame(slotName);
                if (server.isStreamActive(slotName, "pointcloud") && !points.empty()) {
                    server.updatePointCloudData(slotName, points, width, height);
                }
            });
            if (!cam->startStreaming()) {
                fprintf(stderr, "[%s] 热插拔 Orbbec 流启动失败\n", slotName.c_str());
            } else {
                fprintf(stderr, "[%s] 已激活 Orbbec 热插拔流\n", slotName.c_str());
            }
        }

        activatedCameraPtrs[slotName] = cam;
    };

    // ---- 3. 设置夹爪引用（per-slot） ----
    for (auto& slotName : deviceMgr.getGripperSlotNames()) {
        auto* gslot = deviceMgr.getGripperSlot(slotName);
        if (gslot && gslot->connected && gslot->gripperType == "manual") {
            auto* umiPtr = dynamic_cast<UmiGripper*>(gslot->gripper.get());
            if (umiPtr) {
                server.setUmiGripper(slotName, umiPtr);
                fprintf(stderr, "[主程序] 已设置 UMI 夹爪引用 (%s槽)\n", slotName.c_str());
            }
        } else if (gslot && gslot->connected && gslot->gripperType == "electric") {
            auto* ePtr = dynamic_cast<ElectricGripper*>(gslot->gripper.get());
            if (ePtr) {
                server.setElectricGripper(slotName, ePtr);
                fprintf(stderr, "[主程序] 已设置电动夹爪引用 (%s槽)\n", slotName.c_str());
                // 发送初始查询，触发电机返回反馈数据
                ePtr->queryPosition();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                ePtr->queryCurrent();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
    }

    // ---- 打印启动信息 ----
    std::cout << "========================================" << std::endl;
    std::cout << "  服务已启动成功!" << std::endl;

    for (auto& slotName : deviceMgr.getSlotNames()) {
        auto* slot = deviceMgr.getSlot(slotName);
        if (slot && slot->connected && slot->camera) {
            std::cout << "  [" << slotName << "] "
                      << slot->camera->getDeviceType() << ": "
                      << slot->camera->getDeviceName()
                      << " (" << slot->camera->getSerialNumber() << ")"
                      << " depth=" << slot->camera->hasDepthStream()
                      << std::endl;
        } else {
            std::cout << "  [" << slotName << "] 无设备" << std::endl;
        }
    }

    // 打印夹爪信息
    for (auto& slotName : deviceMgr.getGripperSlotNames()) {
        auto* gslot = deviceMgr.getGripperSlot(slotName);
        if (gslot && gslot->connected && gslot->gripper) {
            std::cout << "  [夹爪-" << slotName << "] "
                      << gslot->gripperType << " ("
                      << gslot->gripper->getPortName() << ")" << std::endl;
        }
    }

    std::cout << "  http://localhost:" << cfg.server.port << std::endl;
    std::cout << "========================================" << std::endl;

    // ---- 4. 为每个摄像头启动采集线程 ----
    for (auto& slotName : deviceMgr.getSlotNames()) {
        auto* slot = deviceMgr.getSlot(slotName);
        if (slot && slot->connected && slot->camera && slot->camera->getDeviceType() == "orbbec") {
            if (cfg.slam.enabled && slot->camera->hasDepthStream()) {
                float fx, fy, cx, cy;
                if (slot->camera->getIntrinsics(fx, fy, cx, cy)) {
                    auto slam = std::make_unique<SlamManager>();
                    slam->init(fx, fy, cx, cy, (float)cfg.slam.depthScale);
                    slamManagers[slotName] = std::move(slam);
                } else {
                    fprintf(stderr, "[%s] 无法获取相机内参，跳过 SLAM\n", slotName.c_str());
                }
            }
        }
    }

    for (auto& slotName : deviceMgr.getSlotNames()) {
        auto* slot = deviceMgr.getSlot(slotName);
        if (!slot || !slot->connected || !slot->camera) continue;

        ICamera* cam = slot->camera.get();
        std::string slotPos = slotName;
        int frameSkipMs = cfg.stream.frameSkipMs;

        if (cam->getDeviceType() == "hikvision") {
            // 海康相机：轮询模式
            cameraThreads.emplace_back([cam, &server, slotPos, frameSkipMs]() {
                uint64_t lastTime = 0;
                auto lastFrameTime = std::chrono::steady_clock::now();
                std::string camSerial = cam->getSerialNumber();
                fprintf(stderr, "[%s] 海康相机线程已启动 (SN: %s)\n", slotPos.c_str(), camSerial.c_str());

                while (g_running) {
                    cv::Mat frame = cam->readColor();
                    if (frame.empty()) {
                        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now() - lastFrameTime).count();
                        if (elapsed >= 5) {
                            fprintf(stderr, "[%s] 相机看门狗触发\n", slotPos.c_str());
                            cam->close();
                            std::this_thread::sleep_for(std::chrono::milliseconds(500));
                            if (!cam->open(0, camSerial)) {
                                fprintf(stderr, "[%s] Hikvision camera reconnect failed, thread exits (SN: %s)\n",
                                        slotPos.c_str(), camSerial.c_str());
                                break;
                            }
                            fprintf(stderr, "[%s] 相机重连成功\n", slotPos.c_str());
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                        continue;
                    }

                    lastFrameTime = std::chrono::steady_clock::now();
                    server.tickCaptureFrame(slotPos, "color");

                    if (server.isRecording()) {
                        uint64_t ts = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
                        server.recordFrame(slotPos, "color", frame, ts, frame.cols, frame.rows, "BGR");
                    }

                    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                    if (server.isStreamActive(slotPos, "color") || server.isStreamActive("color")) {
                        if (now - lastTime >= frameSkipMs) {
                            lastTime = now;
                            server.updateColorFrame(slotPos, frame);
                        }
                    }
                }
            });
        } else if (cam->getDeviceType() == "orbbec") {
            // Orbbec 相机：回调模式
            // 注册彩色帧回调
            cam->setColorCallback([&server, slotPos, frameSkipMs, &slamManagers](const cv::Mat& frame, uint64_t timestampUs) {
                static std::map<std::string, uint64_t> lastTimeMap;
                uint64_t& lastTime = lastTimeMap[slotPos];

                server.tickCaptureFrame(slotPos, "color");

                auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();

                if (server.isStreamActive(slotPos, "color") || server.isStreamActive("color")) {
                    if (now - lastTime >= frameSkipMs) {
                        lastTime = now;
                        server.updateColorFrame(slotPos, frame);
                    }
                }

                if (server.isRecording()) {
                    uint64_t ts = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    server.recordFrame(slotPos, "color", frame, ts, frame.cols, frame.rows, "BGR");
                }

                // SLAM：喂入 RGBD 帧（深度帧由深度回调缓存）
                auto slamIt = slamManagers.find(slotPos);
                if (slamIt != slamManagers.end() && slamIt->second) {
                    // 获取最新的深度帧用于 SLAM
                    // 注意：这里简化处理，实际应该用同一帧集的深度数据
                    // 待优化：后续可在同一个帧回调中同时传递 color 和 depth，进一步提升 SLAM 时间同步精度。
                }

                // 更新 SLAM 位姿到服务器
                if (slamIt != slamManagers.end() && slamIt->second && slamIt->second->isInitialized()) {
                    SlamPose pose;
                    slamIt->second->getPose(pose);
                    if (pose.valid) {
                        server.updatePoseData(pose.tx, pose.ty, pose.tz,
                                              pose.qx, pose.qy, pose.qz, pose.qw,
                                              pose.roll, pose.pitch, pose.yaw,
                                              (uint64_t)(pose.timestamp * 1e6));
                    }
                }
            });

            // 注册深度帧回调
            cam->setDepthCallback([&server, slotPos, frameSkipMs, cfg](const cv::Mat& visualization, const cv::Mat& rawDepth, uint64_t timestampUs) {
                static std::map<std::string, uint64_t> lastTimeMap;
                uint64_t& lastTime = lastTimeMap[slotPos];

                server.tickCaptureFrame(slotPos, "depth");

                auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();

                if (server.isStreamActive(slotPos, "depth") && now - lastTime >= frameSkipMs) {
                    lastTime = now;
                    server.updateDepthFrame(slotPos, visualization);
                }

                // 录制原始深度帧：保存 Y16 深度数据，预览图只用于网页显示。
                if (server.isRecording() && !rawDepth.empty()) {
                    uint64_t ts = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    server.recordFrame(slotPos, "depth", rawDepth, ts, rawDepth.cols, rawDepth.rows, "Y16");
                }
            });

            // 注册左红外帧回调
            cam->setIRLeftCallback([&server, slotPos, frameSkipMs](const cv::Mat& irFrame, uint64_t timestampUs) {
                static std::map<std::string, uint64_t> lastTimeMap;
                uint64_t& lastTime = lastTimeMap[slotPos + "_irl"];
                server.tickCaptureFrame(slotPos, "ir-left");
                auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                if (server.isStreamActive(slotPos, "ir-left") && now - lastTime >= frameSkipMs) {
                    lastTime = now;
                    server.updateIRLeftFrame(slotPos, irFrame);
                }
                // 录制左红外帧：按统一会话时间戳写入 ir-left_video。
                if (server.isRecording() && !irFrame.empty()) {
                    uint64_t ts = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    server.recordFrame(slotPos, "ir-left", irFrame, ts, irFrame.cols, irFrame.rows, "Y8");
                }
            });
            // 注册右红外帧回调
            cam->setIRRightCallback([&server, slotPos, frameSkipMs](const cv::Mat& irFrame, uint64_t timestampUs) {
                static std::map<std::string, uint64_t> lastTimeMap;
                uint64_t& lastTime = lastTimeMap[slotPos + "_irr"];
                server.tickCaptureFrame(slotPos, "ir-right");
                auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                if (server.isStreamActive(slotPos, "ir-right") && now - lastTime >= frameSkipMs) {
                    lastTime = now;
                    server.updateIRRightFrame(slotPos, irFrame);
                }
                // 录制右红外帧：按统一会话时间戳写入 ir-right_video。
                if (server.isRecording() && !irFrame.empty()) {
                    uint64_t ts = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    server.recordFrame(slotPos, "ir-right", irFrame, ts, irFrame.cols, irFrame.rows, "Y8");
                }
            });

            // 注册点云回调（仅 Orbbec 相机支持，通过基类接口）
            if (cam->getDeviceType() == "orbbec") {
                cam->setPointCloudCallback([&server, slotPos](const std::vector<float>& points, int width, int height, uint64_t timestampUs) {
                    if (!points.empty()) server.tickPointCloudFrame(slotPos);
                    if (server.isStreamActive(slotPos, "pointcloud") && !points.empty()) {
                        server.updatePointCloudData(slotPos, points, width, height);
                    }
                });
                fprintf(stderr, "[%s] 点云回调已注册\n", slotPos.c_str());
            }

            // 启动 Orbbec 流
            if (!cam->startStreaming()) {
                fprintf(stderr, "[%s] Orbbec 流启动失败\n", slotPos.c_str());
            }
        }
    }

    for (auto& slotName : deviceMgr.getSlotNames()) {
        auto* slot = deviceMgr.getSlot(slotName);
        if (slot && slot->connected && slot->camera) {
            activatedCameraPtrs[slotName] = slot->camera.get();
        }
    }
    for (auto& slotName : deviceMgr.getGripperSlotNames()) {
        auto* gslot = deviceMgr.getGripperSlot(slotName);
        if (gslot && gslot->connected && gslot->gripper) {
            activatedGripperPtrs[slotName] = gslot->gripper.get();
        }
    }
    std::thread hotplugThread([&]() {
        while (g_running) {
            for (auto& slotName : deviceMgr.getSlotNames()) activateCameraSlot(slotName);
            for (auto& slotName : deviceMgr.getGripperSlotNames()) activateGripperSlot(slotName);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    });

    // ---- 5. 夹爪数据推送线程 ----
    std::thread gripperThread([&]() {
        int queryTick = 0;
        while (g_running) {
            queryTick++;
            for (auto& slotName : deviceMgr.getGripperSlotNames()) {
                auto* gslot = deviceMgr.getGripperSlot(slotName);
                if (gslot && gslot->connected && gslot->gripper) {
                    if (gslot->gripperType == "electric") {
                        auto* eGripper = dynamic_cast<ElectricGripper*>(gslot->gripper.get());
                        if (eGripper) {
                            // 每 500ms 自动查询一次位置/速度/电流（25 ticks * 20ms = 500ms）
                            if (queryTick % 25 == 0) {
                                eGripper->queryPosition();
                                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                                eGripper->querySpeed();
                                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                                eGripper->queryCurrent();
                            }
                            ElectricGripperFullState eState;
                            eGripper->getFullState(eState);
                            eState.connected = eGripper->isConnected();
                            server.updateElectricGripperData(slotName, eState);
                            if (eState.hasData) {
                                GripperState gs;
                                eGripper->getState(gs);
                                server.updateGripperData(slotName, gs.position, gs.button1, gs.button2, gs.timestamp);
                                if (server.isRecording()) {
                                    // 使用录制时刻的系统时间，避免 CAN 缓存的陈旧时间戳
                                    uint64_t recordTs = std::chrono::duration_cast<std::chrono::microseconds>(
                                        std::chrono::system_clock::now().time_since_epoch()).count();
                                    server.recordElectricGripper(slotName, eState.positionDeg,
                                        eState.velocity, eState.current,
                                        eState.motorTemp, eState.mosTemp,
                                        eState.errorCode, recordTs);
                                }
                            }
                        }
                    } else {
                        GripperState gs;
                        gslot->gripper->getState(gs);
                        if (gs.hasData) {
                            server.updateGripperData(slotName, gs.position, gs.button1, gs.button2, gs.timestamp);
                            if (server.isRecording()) {
                                server.recordGripper(slotName, gs.position, gs.button1, gs.button2, gs.timestamp);
                            }
                        }
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });

    // ---- 6. 等待退出 ----
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // ---- 7. 清理 ----
    // 关闭所有夹爪
    for (auto& slotName : deviceMgr.getGripperSlotNames()) {
        auto* gslot = deviceMgr.getGripperSlot(slotName);
        if (gslot && gslot->gripper) {
            gslot->gripper->close();
        }
    }
    if (hotplugThread.joinable()) hotplugThread.join();
    if (gripperThread.joinable()) gripperThread.join();

    // 停止所有 Orbbec 流
    for (auto& slotName : deviceMgr.getSlotNames()) {
        auto* slot = deviceMgr.getSlot(slotName);
        if (slot && slot->connected && slot->camera) {
            slot->camera->stopStreaming();
        }
    }

    for (auto& t : cameraThreads) {
        if (t.joinable()) t.join();
    }

    server.stop();

    } catch (std::exception& e) {
        std::cerr << "启动失败: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
