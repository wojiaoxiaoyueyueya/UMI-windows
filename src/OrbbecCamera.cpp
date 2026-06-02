// OrbbecCamera.cpp - Orbbec Gemini 305 深度相机实现
// 使用 OrbbecSDK v2.8.6 C API

#include "OrbbecCamera.hpp"

#ifndef NO_ORBBEC_CAMERA

#include <cstdio>
#include <cstring>
#include <chrono>
#include <windows.h>

#include <libobsensor/ObSensor.h>

// ---- 构造/析构 ----

OrbbecCamera::OrbbecCamera() = default;

OrbbecCamera::~OrbbecCamera() {
    stopStreaming();
    close();
}

// ---- 打开/关闭 ----

bool OrbbecCamera::open(int deviceIndex, const std::string& /*serial*/) {
    ob_error* err = nullptr;

    if (!context_) {
        context_ = ob_create_context(&err);
        if (err || !context_) {
            fprintf(stderr, "[Orbbec] 创建 Context 失败: %s\n",
                    err ? ob_error_get_message(err) : "unknown");
            if (err) ob_delete_error(err);
            return false;
        }
    }

    auto* devList = ob_query_device_list(context_, &err);
    if (err || !devList) {
        fprintf(stderr, "[Orbbec] 查询设备列表失败: %s\n",
                err ? ob_error_get_message(err) : "unknown");
        if (err) ob_delete_error(err);
        return false;
    }

    uint32_t devCount = ob_device_list_get_count(devList, &err);
    if (err) { ob_delete_error(err); ob_delete_device_list(devList, &err); return false; }

    if ((uint32_t)deviceIndex >= devCount) {
        fprintf(stderr, "[Orbbec] 设备索引 %d 超出范围 (共 %u 个设备)\n", deviceIndex, devCount);
        ob_delete_device_list(devList, &err);
        return false;
    }

    device_ = ob_device_list_get_device(devList, deviceIndex, &err);
    ob_delete_device_list(devList, &err);
    if (err || !device_) {
        fprintf(stderr, "[Orbbec] 获取设备失败: %s\n", err ? ob_error_get_message(err) : "unknown");
        if (err) ob_delete_error(err);
        return false;
    }

    auto* devInfo = ob_device_get_device_info(device_, &err);
    if (!err && devInfo) {
        const char* name = ob_device_info_get_name(devInfo, &err);
        if (!err && name) deviceName_ = name;
        if (err) { ob_delete_error(err); err = nullptr; }

        const char* sn = ob_device_info_get_serial_number(devInfo, &err);
        if (!err && sn) serialNumber_ = sn;
        if (err) { ob_delete_error(err); err = nullptr; }

        ob_delete_device_info(devInfo, &err);
    }
    if (err) ob_delete_error(err);

    pipeline_ = ob_create_pipeline_with_device(device_, &err);
    if (err || !pipeline_) {
        fprintf(stderr, "[Orbbec] 创建 Pipeline 失败: %s\n",
                err ? ob_error_get_message(err) : "unknown");
        if (err) ob_delete_error(err);
        return false;
    }

    config_ = ob_create_config(&err);
    if (err || !config_) {
        fprintf(stderr, "[Orbbec] 创建 Config 失败\n");
        if (err) ob_delete_error(err);
        return false;
    }

    initCapabilities();

    opened_ = true;
    fprintf(stderr, "[Orbbec] 设备已打开: %s (SN: %s)\n",
            deviceName_.c_str(), serialNumber_.c_str());
    return true;
}

void OrbbecCamera::close() {
    if (!opened_) return;
    stopStreaming();

    ob_error* err = nullptr;
    if (config_) { ob_delete_config(config_, &err); config_ = nullptr; }
    if (err) { ob_delete_error(err); err = nullptr; }
    if (pipeline_) { ob_delete_pipeline(pipeline_, &err); pipeline_ = nullptr; }
    if (err) { ob_delete_error(err); err = nullptr; }
    device_ = nullptr;
    opened_ = false;
    fprintf(stderr, "[Orbbec] 设备已关闭\n");
}

bool OrbbecCamera::isOpened() const { return opened_; }

// ---- 能力检测 ----

void OrbbecCamera::initCapabilities() {
    if (!device_) return;
    ob_error* err = nullptr;

    auto* sensorList = ob_device_get_sensor_list(device_, &err);
    if (err || !sensorList) { if (err) ob_delete_error(err); return; }

    uint32_t count = ob_sensor_list_get_sensor_count(sensorList, &err);
    if (err) { ob_delete_error(err); ob_delete_sensor_list(sensorList, &err); return; }

    for (uint32_t i = 0; i < count; i++) {
        auto type = ob_sensor_list_get_sensor_type(sensorList, i, &err);
        if (err) { ob_delete_error(err); continue; }

        if (type == OB_SENSOR_COLOR) hasColor_ = true;
        else if (type == OB_SENSOR_DEPTH) hasDepth_ = true;
        else if (type == OB_SENSOR_IR || type == OB_SENSOR_IR_LEFT || type == OB_SENSOR_IR_RIGHT) hasIR_ = true;
        else if (type == OB_SENSOR_ACCEL || type == OB_SENSOR_GYRO) { hasIMU_ = true; }
    }
    ob_delete_sensor_list(sensorList, &err);

    // 相机内参
    if (pipeline_) {
        ob_camera_param cameraParam = {};
        cameraParam = ob_pipeline_get_camera_param(pipeline_, &err);
        if (!err) {
            fx_ = cameraParam.rgbIntrinsic.fx;
            fy_ = cameraParam.rgbIntrinsic.fy;
            cx_ = cameraParam.rgbIntrinsic.cx;
            cy_ = cameraParam.rgbIntrinsic.cy;
            hasIntrinsics_ = (fx_ > 0 && fy_ > 0);
        }
        if (err) ob_delete_error(err);
    }

    fprintf(stderr, "[Orbbec] 能力: color=%d depth=%d ir=%d imu=%d intrinsics=%d\n",
            hasColor_, hasDepth_, hasIR_, hasIMU_, hasIntrinsics_);
}

// ---- 流控制 ----

bool OrbbecCamera::startStreaming() {
    if (!opened_ || !pipeline_ || !config_) return false;
    if (streaming_) return true;

    ob_error* err = nullptr;

    // 等齐所有帧类型才输出，确保 IR_LEFT/IR_RIGHT 帧在 frameset 中
    ob_config_set_frame_aggregate_output_mode(config_, OB_FRAME_AGGREGATE_OUTPUT_ALL_TYPE_FRAME_REQUIRE, &err);
    if (err) { ob_delete_error(err); err = nullptr; }

    if (hasColor_) {
        auto* profiles = ob_pipeline_get_stream_profile_list(pipeline_, OB_SENSOR_COLOR, &err);
        if (!err && profiles) {
            // 优先选择 640x480 降低缓冲区大小，避免 Alloc frame buffer failed
            auto* profile = ob_stream_profile_list_get_video_stream_profile(
                profiles, 640, 480, OB_FORMAT_UNKNOWN, 0, &err);
            if (err || !profile) {
                if (err) { ob_delete_error(err); err = nullptr; }
                profile = ob_stream_profile_list_get_profile(profiles, 0, &err);
            }
            if (!err && profile) {
                ob_config_enable_stream_with_stream_profile(config_, profile, &err);
                int w = ob_video_stream_profile_get_width(profile, &err);
                if (err) { ob_delete_error(err); err = nullptr; }
                int h = ob_video_stream_profile_get_height(profile, &err);
                if (err) { ob_delete_error(err); err = nullptr; }
                auto fmt = ob_stream_profile_get_format(profile, &err);
                if (err) { ob_delete_error(err); err = nullptr; }
                fprintf(stderr, "[Orbbec] Color: %dx%d fmt=%d\n", w, h, (int)fmt);
            }
            ob_delete_stream_profile_list(profiles, &err);
        }
        if (err) ob_delete_error(err);
    }

    if (hasDepth_) {
        err = nullptr;
        auto* profiles = ob_pipeline_get_stream_profile_list(pipeline_, OB_SENSOR_DEPTH, &err);
        if (!err && profiles) {
            auto* profile = ob_stream_profile_list_get_video_stream_profile(
                profiles, 640, 480, OB_FORMAT_UNKNOWN, 0, &err);
            if (err || !profile) {
                if (err) { ob_delete_error(err); err = nullptr; }
                profile = ob_stream_profile_list_get_profile(profiles, 0, &err);
            }
            if (!err && profile) {
                ob_config_enable_stream_with_stream_profile(config_, profile, &err);
                int w = ob_video_stream_profile_get_width(profile, &err);
                if (err) { ob_delete_error(err); err = nullptr; }
                int h = ob_video_stream_profile_get_height(profile, &err);
                if (err) { ob_delete_error(err); err = nullptr; }
                fprintf(stderr, "[Orbbec] Depth: %dx%d\n", w, h);
            }
            ob_delete_stream_profile_list(profiles, &err);
        }
        if (err) ob_delete_error(err);
    }

    if (hasIR_) {
        err = nullptr;
        // 尝试分别启用左红外和右红外（双目相机）
        bool stereoIR = false;
        auto* profilesL = ob_pipeline_get_stream_profile_list(pipeline_, OB_SENSOR_IR_LEFT, &err);
        if (!err && profilesL && ob_stream_profile_list_count(profilesL, &err) > 0) {
            auto* profile = ob_stream_profile_list_get_video_stream_profile(
                profilesL, 640, 480, OB_FORMAT_UNKNOWN, 0, &err);
            if (err || !profile) {
                if (err) { ob_delete_error(err); err = nullptr; }
                profile = ob_stream_profile_list_get_profile(profilesL, 0, &err);
            }
            if (!err && profile) {
                ob_config_enable_stream_with_stream_profile(config_, profile, &err);
                fprintf(stderr, "[Orbbec] IR-Left enabled\n");
                stereoIR = true;
            }
            ob_delete_stream_profile_list(profilesL, &err);
        }
        if (err) ob_delete_error(err);

        err = nullptr;
        auto* profilesR = ob_pipeline_get_stream_profile_list(pipeline_, OB_SENSOR_IR_RIGHT, &err);
        if (!err && profilesR && ob_stream_profile_list_count(profilesR, &err) > 0) {
            auto* profile = ob_stream_profile_list_get_video_stream_profile(
                profilesR, 640, 480, OB_FORMAT_UNKNOWN, 0, &err);
            if (err || !profile) {
                if (err) { ob_delete_error(err); err = nullptr; }
                profile = ob_stream_profile_list_get_profile(profilesR, 0, &err);
            }
            if (!err && profile) {
                ob_config_enable_stream_with_stream_profile(config_, profile, &err);
                fprintf(stderr, "[Orbbec] IR-Right enabled\n");
            }
            ob_delete_stream_profile_list(profilesR, &err);
        }
        if (err) ob_delete_error(err);

        // 如果没有独立的左右红外传感器，回退到通用 IR
        if (!stereoIR) {
            err = nullptr;
            auto* profiles = ob_pipeline_get_stream_profile_list(pipeline_, OB_SENSOR_IR, &err);
            if (!err && profiles) {
                auto* profile = ob_stream_profile_list_get_video_stream_profile(
                    profiles, 640, 480, OB_FORMAT_UNKNOWN, 0, &err);
                if (err || !profile) {
                    if (err) { ob_delete_error(err); err = nullptr; }
                    profile = ob_stream_profile_list_get_profile(profiles, 0, &err);
                }
                if (!err && profile) {
                    ob_config_enable_stream_with_stream_profile(config_, profile, &err);
                    int w = ob_video_stream_profile_get_width(profile, &err);
                    if (err) { ob_delete_error(err); err = nullptr; }
                    int h = ob_video_stream_profile_get_height(profile, &err);
                    if (err) { ob_delete_error(err); err = nullptr; }
                    fprintf(stderr, "[Orbbec] IR (mono): %dx%d\n", w, h);
                }
                ob_delete_stream_profile_list(profiles, &err);
            }
            if (err) ob_delete_error(err);
        }
    }

    ob_pipeline_start_with_callback(pipeline_, config_, frameSetCallback, this, &err);
    if (err) {
        fprintf(stderr, "[Orbbec] 启动 Pipeline 失败: %s\n", ob_error_get_message(err));
        ob_delete_error(err);
        return false;
    }

    // 创建点云 filter
    err = nullptr;
    pointCloudFilter_ = ob_create_filter("PointCloudFilter", &err);
    if (err || !pointCloudFilter_) {
        fprintf(stderr, "[Orbbec] 创建 PointCloudFilter 失败: %s\n", err ? ob_error_get_message(err) : "unknown");
        if (err) ob_delete_error(err);
        err = nullptr;
    } else {
        ob_filter_set_config_value(pointCloudFilter_, "pointFormat", 20.0, &err);
        if (err) { ob_delete_error(err); err = nullptr; }
        ob_filter_set_config_value(pointCloudFilter_, "decimate", 2.0, &err);
        if (err) { ob_delete_error(err); err = nullptr; }
        fprintf(stderr, "[Orbbec] PointCloudFilter 已创建\n");
    }

    // 创建深度对齐 filter
    err = nullptr;
    alignFilter_ = ob_create_filter("Align", &err);
    if (err || !alignFilter_) {
        fprintf(stderr, "[Orbbec] 创建 AlignFilter 失败: %s\n", err ? ob_error_get_message(err) : "unknown");
        if (err) ob_delete_error(err);
        err = nullptr;
    } else {
        ob_filter_set_config_value(alignFilter_, "AlignType", (double)OB_STREAM_COLOR, &err);
        if (err) { ob_delete_error(err); err = nullptr; }
        fprintf(stderr, "[Orbbec] AlignFilter 已创建\n");
    }

    streaming_ = true;
    fprintf(stderr, "[Orbbec] Pipeline 已启动\n");

    return true;
}

void OrbbecCamera::stopStreaming() {
    if (!streaming_ || !pipeline_) return;

    ob_error* err = nullptr;
    if (pointCloudFilter_) {
        ob_delete_filter(pointCloudFilter_, &err);
        if (err) { ob_delete_error(err); err = nullptr; }
        pointCloudFilter_ = nullptr;
    }
    if (alignFilter_) {
        ob_delete_filter(alignFilter_, &err);
        if (err) { ob_delete_error(err); err = nullptr; }
        alignFilter_ = nullptr;
    }

    ob_pipeline_stop(pipeline_, &err);
    if (err) { fprintf(stderr, "[Orbbec] 停止 Pipeline 失败: %s\n", ob_error_get_message(err)); ob_delete_error(err); }
    streaming_ = false;
    fprintf(stderr, "[Orbbec] Pipeline 已停止\n");
}

// ---- 帧回调 ----

void OrbbecCamera::frameSetCallback(ob_frame* frameset, void* userdata) {
    auto* self = static_cast<OrbbecCamera*>(userdata);
    if (!self) return;
    try {
        self->onFrameSetCallback(frameset);
    } catch (const std::exception& e) {
        fprintf(stderr, "[Orbbec] 帧回调异常: %s\n", e.what());
    } catch (...) {
        fprintf(stderr, "[Orbbec] 帧回调未知异常\n");
    }
}

void OrbbecCamera::onFrameSetCallback(ob_frame* frameset) {
    if (!frameset) return;
    ob_error* err = nullptr;

    // Phase 0: 在提取任何子帧之前生成点云（frameset 必须完整）
    if (pointCloudCb_ && pointCloudFilter_ && alignFilter_) {
        err = nullptr;
        ob_frame* aligned = ob_filter_process(alignFilter_, frameset, &err);
        if (err) {
            static int alignFailCount = 0;
            if (++alignFailCount <= 5) fprintf(stderr, "[Orbbec] PointCloud align failed: %s\n", ob_error_get_message(err));
            ob_delete_error(err);
            err = nullptr;
        } else if (aligned) {
            err = nullptr;
            ob_frame* pcFrame = ob_filter_process(pointCloudFilter_, aligned, &err);
            if (err) {
                static int pcFailCount = 0;
                if (++pcFailCount <= 5) fprintf(stderr, "[Orbbec] PointCloud filter failed: %s\n", ob_error_get_message(err));
                ob_delete_error(err);
                err = nullptr;
            } else if (pcFrame) {
                void* pcData = ob_frame_get_data(pcFrame, &err);
                uint32_t pcSize = ob_frame_get_data_size(pcFrame, &err);
                if (!err && pcData && pcSize > 0) {
                    // 获取坐标缩放因子（原始数据需乘以 scale 得到毫米单位）
                    err = nullptr;
                    float scale = ob_points_frame_get_coordinate_value_scale(pcFrame, &err);
                    if (err) { ob_delete_error(err); scale = 1.0f; err = nullptr; }

                    // OB_FORMAT_RGB_POINT (20): 每个 OBColorPoint = {x,y,z,r,g,b} 各 float
                    int pointCount = pcSize / sizeof(OBColorPoint);
                    if (pointCount > 0) {
                        auto* srcPts = (OBColorPoint*)pcData;
                        std::vector<float> pts(pointCount * 6);
                        for (int j = 0; j < pointCount; j++) {
                            pts[j*6+0] = srcPts[j].x * scale;
                            pts[j*6+1] = srcPts[j].y * scale;
                            pts[j*6+2] = srcPts[j].z * scale;
                            pts[j*6+3] = srcPts[j].b;  // BGR → RGB: 交换 R 和 B
                            pts[j*6+4] = srcPts[j].g;
                            pts[j*6+5] = srcPts[j].r;
                        }
                        int pcW = 0, pcH = 0;
                        err = nullptr;
                        pcW = ob_point_cloud_frame_get_width(pcFrame, &err);
                        if (err) { ob_delete_error(err); pcW = 0; err = nullptr; }
                        pcH = ob_point_cloud_frame_get_height(pcFrame, &err);
                        if (err) { ob_delete_error(err); pcH = 0; err = nullptr; }
                        pointCloudCb_(pts, pcW, pcH, 0);
                        static int pcSuccessCount = 0;
                        if (++pcSuccessCount <= 3) fprintf(stderr, "[Orbbec] PointCloud OK: %d points (scale=%.4f), %dx%d\n", pointCount, scale, pcW, pcH);
                    }
                }
                if (err) ob_delete_error(err);
                ob_delete_frame(pcFrame, &err);
                if (err) ob_delete_error(err);
            }
            ob_delete_frame(aligned, &err);
            if (err) ob_delete_error(err);
        }
    }

    // Phase 1: 快速复制原始数据并立即释放帧，不让 SDK 缓冲池耗尽
    std::vector<uchar> colorRawBuf;
    int colorW = 0, colorH = 0;
    OBFormat colorFormat = OB_FORMAT_UNKNOWN;
    uint64_t colorTs = 0;
    bool hasColor = false;

    cv::Mat depthRawCopy;
    int depthW = 0, depthH = 0;
    uint64_t depthTs = 0;
    bool hasDepth = false;

    cv::Mat irLeftRawCopy;
    uint64_t irLeftTs = 0;
    bool hasIRLeft = false;
    cv::Mat irRightRawCopy;
    uint64_t irRightTs = 0;
    bool hasIRRight = false;

    if (colorCb_) {
        auto* colorFrame = ob_frameset_get_color_frame(frameset, &err);
        if (!err && colorFrame) {
            colorTs = ob_frame_get_timestamp_us(colorFrame, &err);
            if (err) { ob_delete_error(err); colorTs = 0; err = nullptr; }
            colorFormat = ob_frame_get_format(colorFrame, &err);
            if (err) { ob_delete_error(err); colorFormat = OB_FORMAT_UNKNOWN; err = nullptr; }
            colorW = ob_video_frame_get_width(colorFrame, &err);
            if (err) { ob_delete_error(err); err = nullptr; colorW = 0; }
            colorH = ob_video_frame_get_height(colorFrame, &err);
            if (err) { ob_delete_error(err); err = nullptr; colorH = 0; }
            void* data = ob_frame_get_data(colorFrame, &err);
            uint32_t dataSize = ob_frame_get_data_size(colorFrame, &err);
            if (!err && data && dataSize > 0) {
                colorRawBuf.assign((uchar*)data, (uchar*)data + dataSize);
                hasColor = true;
            }
            if (err) ob_delete_error(err);
            ob_delete_frame(colorFrame, &err);  // 立即释放子帧
        }
        if (err) ob_delete_error(err);
    }

    if (depthCb_) {
        err = nullptr;
        auto* depthFrame = ob_frameset_get_depth_frame(frameset, &err);
        if (!err && depthFrame) {
            depthTs = ob_frame_get_timestamp_us(depthFrame, &err);
            if (err) { ob_delete_error(err); depthTs = 0; err = nullptr; }
            depthW = ob_video_frame_get_width(depthFrame, &err);
            if (err) { ob_delete_error(err); err = nullptr; depthW = 0; }
            depthH = ob_video_frame_get_height(depthFrame, &err);
            if (err) { ob_delete_error(err); err = nullptr; depthH = 0; }
            void* data = ob_frame_get_data(depthFrame, &err);
            if (!err && data && depthW > 0 && depthH > 0) {
                depthRawCopy.create(depthH, depthW, CV_16UC1);
                memcpy(depthRawCopy.data, data, depthW * depthH * 2);
                hasDepth = true;
            }
            if (err) ob_delete_error(err);
            ob_delete_frame(depthFrame, &err);  // 立即释放子帧
        }
        if (err) ob_delete_error(err);
    }

    if (irLeftCb_ || irRightCb_) {
        err = nullptr;
        // 左红外
        if (irLeftCb_) {
            auto* irLeftFrame = ob_frameset_get_frame(frameset, OB_FRAME_IR_LEFT, &err);
            if (!err && irLeftFrame) {
                irLeftTs = ob_frame_get_timestamp_us(irLeftFrame, &err);
                if (err) { ob_delete_error(err); irLeftTs = 0; err = nullptr; }
                irLeftRawCopy = convertIRFrame(irLeftFrame);
                hasIRLeft = !irLeftRawCopy.empty();
                ob_delete_frame(irLeftFrame, &err);
            } else {
                static int irlFailCount = 0;
                if (++irlFailCount <= 3) fprintf(stderr, "[Orbbec] IR-Left frame missing: %s\n", err ? ob_error_get_message(err) : "null frame");
                if (err) ob_delete_error(err);
            }
        }
        // 右红外
        if (irRightCb_) {
            err = nullptr;
            auto* irRightFrame = ob_frameset_get_frame(frameset, OB_FRAME_IR_RIGHT, &err);
            if (!err && irRightFrame) {
                irRightTs = ob_frame_get_timestamp_us(irRightFrame, &err);
                if (err) { ob_delete_error(err); irRightTs = 0; err = nullptr; }
                irRightRawCopy = convertIRFrame(irRightFrame);
                hasIRRight = !irRightRawCopy.empty();
                ob_delete_frame(irRightFrame, &err);
            } else {
                static int irrFailCount = 0;
                if (++irrFailCount <= 3) fprintf(stderr, "[Orbbec] IR-Right frame missing: %s\n", err ? ob_error_get_message(err) : "null frame");
                if (err) ob_delete_error(err);
            }
        }
    }

    // 释放 frameset 本身！否则 SDK 缓冲池会耗尽导致 Alloc frame buffer failed
    ob_delete_frame(frameset, nullptr);

    // Phase 2: 在副本上做耗时处理（SDK 帧已全部释放）
    if (hasColor && !colorRawBuf.empty()) {
        cv::Mat color;
        if (colorFormat == OB_FORMAT_MJPG) {
            color = cv::imdecode(colorRawBuf, cv::IMREAD_COLOR);
        } else if (colorFormat == OB_FORMAT_RGB && colorW > 0 && colorH > 0) {
            cv::Mat rgb(colorH, colorW, CV_8UC3, colorRawBuf.data());
            cv::cvtColor(rgb, color, cv::COLOR_RGB2BGR);
        } else if (colorFormat == OB_FORMAT_BGR && colorW > 0 && colorH > 0) {
            color = cv::Mat(colorH, colorW, CV_8UC3, colorRawBuf.data()).clone();
        } else if (colorFormat == OB_FORMAT_BGRA && colorW > 0 && colorH > 0) {
            cv::Mat bgra(colorH, colorW, CV_8UC4, colorRawBuf.data());
            cv::cvtColor(bgra, color, cv::COLOR_BGRA2BGR);
        } else if (colorFormat == OB_FORMAT_RGBA && colorW > 0 && colorH > 0) {
            cv::Mat rgba(colorH, colorW, CV_8UC4, colorRawBuf.data());
            cv::cvtColor(rgba, color, cv::COLOR_RGBA2BGR);
        } else if ((colorFormat == OB_FORMAT_YUYV || colorFormat == OB_FORMAT_Y16 || colorFormat == OB_FORMAT_Y8) && colorW > 0 && colorH > 0) {
            cv::Mat yuv(colorH, colorW, CV_8UC2, colorRawBuf.data());
            cv::cvtColor(yuv, color, cv::COLOR_YUV2BGR_YUYV);
        }
        if (!color.empty()) colorCb_(color, colorTs);
    }

    if (hasDepth && !depthRawCopy.empty()) {
        double minVal, maxVal;
        cv::minMaxLoc(depthRawCopy, &minVal, &maxVal);
        cv::Mat depth8;
        if (maxVal > 0) depthRawCopy.convertTo(depth8, CV_8UC1, 255.0 / maxVal);
        else depth8 = cv::Mat::zeros(depthH, depthW, CV_8UC1);
        cv::Mat colorDepth;
        cv::applyColorMap(depth8, colorDepth, cv::COLORMAP_JET);
        if (!colorDepth.empty()) depthCb_(colorDepth, depthRawCopy, depthTs);
    }

    if (hasIRLeft && !irLeftRawCopy.empty()) {
        irLeftCb_(irLeftRawCopy, irLeftTs);
    }

    if (hasIRRight && !irRightRawCopy.empty()) {
        irRightCb_(irRightRawCopy, irRightTs);
    }
}

// ---- 帧格式转换 ----

cv::Mat OrbbecCamera::convertColorFrame(ob_frame* frame) {
    ob_error* err = nullptr;
    auto format = ob_frame_get_format(frame, &err);
    if (err) { ob_delete_error(err); return cv::Mat(); }

    int w = ob_video_frame_get_width(frame, &err);
    int h = ob_video_frame_get_height(frame, &err);
    if (err) { ob_delete_error(err); return cv::Mat(); }

    void* data = ob_frame_get_data(frame, &err);
    if (err || !data) { if (err) ob_delete_error(err); return cv::Mat(); }

    cv::Mat result;
    switch (format) {
        case OB_FORMAT_MJPG: {
            uint32_t dataSize = ob_frame_get_data_size(frame, &err);
            if (err) { ob_delete_error(err); return cv::Mat(); }
            std::vector<uchar> buf((uchar*)data, (uchar*)data + dataSize);
            result = cv::imdecode(buf, cv::IMREAD_COLOR);
            break;
        }
        case OB_FORMAT_RGB: {
            cv::Mat rgb(h, w, CV_8UC3, data);
            cv::cvtColor(rgb, result, cv::COLOR_RGB2BGR);
            break;
        }
        case OB_FORMAT_BGR:
            result = cv::Mat(h, w, CV_8UC3, data).clone();
            break;
        default:
            break;
    }
    return result;
}

cv::Mat OrbbecCamera::convertDepthFrame(ob_frame* frame) {
    ob_error* err = nullptr;
    int w = ob_video_frame_get_width(frame, &err);
    int h = ob_video_frame_get_height(frame, &err);
    if (err) { ob_delete_error(err); return cv::Mat(); }

    void* data = ob_frame_get_data(frame, &err);
    if (err || !data) { if (err) ob_delete_error(err); return cv::Mat(); }

    cv::Mat depthRaw(h, w, CV_16UC1, data);
    double minVal, maxVal;
    cv::minMaxLoc(depthRaw, &minVal, &maxVal);
    cv::Mat depth8;
    if (maxVal > 0) depthRaw.convertTo(depth8, CV_8UC1, 255.0 / maxVal);
    else depth8 = cv::Mat::zeros(h, w, CV_8UC1);

    cv::Mat colorDepth;
    cv::applyColorMap(depth8, colorDepth, cv::COLORMAP_JET);
    return colorDepth;
}

cv::Mat OrbbecCamera::convertIRFrame(ob_frame* frame) {
    ob_error* err = nullptr;
    int w = ob_video_frame_get_width(frame, &err);
    int h = ob_video_frame_get_height(frame, &err);
    if (err) { ob_delete_error(err); return cv::Mat(); }

    void* data = ob_frame_get_data(frame, &err);
    if (err || !data) { if (err) ob_delete_error(err); return cv::Mat(); }

    auto format = ob_frame_get_format(frame, &err);
    if (err) { ob_delete_error(err); return cv::Mat(); }

    cv::Mat result;
    if (format == OB_FORMAT_Y16) {
        cv::Mat ir16(h, w, CV_16UC1, data);
        ir16.convertTo(result, CV_8UC1, 1.0 / 256.0);
    } else if (format == OB_FORMAT_Y8) {
        result = cv::Mat(h, w, CV_8UC1, data).clone();
    }
    return result;
}

// ---- 内参 ----

bool OrbbecCamera::getIntrinsics(float& fx, float& fy, float& cx, float& cy) const {
    if (!hasIntrinsics_) return false;
    fx = fx_; fy = fy_; cx = cx_; cy = cy_;
    return true;
}

// ---- 设备信息 ----

std::string OrbbecCamera::getDeviceName() const { return deviceName_.empty() ? "Orbbec Camera" : deviceName_; }
std::string OrbbecCamera::getSerialNumber() const { return serialNumber_.empty() ? "N/A" : serialNumber_; }

#endif // NO_ORBBEC_CAMERA
