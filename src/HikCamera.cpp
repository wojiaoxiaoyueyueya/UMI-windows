// HikCamera.cpp - 海康工业相机采集实现
// 支持多实例：每个 HikCamera 打开指定 index 的设备，帧缓冲区独立

#include "HikCamera.hpp"
#ifndef NO_HIK_CAMERA
#include <MvCameraControl.h>
#endif
#include <cstdio>
#include <cstring>

#ifndef NO_HIK_CAMERA

// 每个实例独立的帧缓冲区
struct HikCamera::RawFrameBuffer {
    std::mutex mutex;
    std::vector<uint8_t> data;
    int width = 0;
    int height = 0;
    unsigned int pixelType = 0;
    size_t frameLen = 0;
    std::atomic<bool> hasNew{false};
};

static void __stdcall imageCallback(unsigned char* pData, MV_FRAME_OUT_INFO_EX* pFrameInfo, void* pUser) {
    if(!pData || !pFrameInfo || !pUser) return;
    auto* buf = static_cast<HikCamera::RawFrameBuffer*>(pUser);
    try {
        size_t dataSize = pFrameInfo->nFrameLen;
        std::lock_guard<std::mutex> lock(buf->mutex);
        if(buf->data.size() < dataSize) buf->data.resize(dataSize);
        memcpy(buf->data.data(), pData, dataSize);
        buf->width = pFrameInfo->nWidth;
        buf->height = pFrameInfo->nHeight;
        buf->pixelType = pFrameInfo->enPixelType;
        buf->frameLen = dataSize;
        buf->hasNew = true;
    } catch (...) {
        fprintf(stderr, "[海康相机] 帧回调异常\n");
    }
}

HikCamera::HikCamera(const CameraConfig& cfg) : handle_(nullptr), opened_(false), cfg_(cfg) {
    rawFrame_ = std::make_unique<RawFrameBuffer>();
}

HikCamera::~HikCamera() { close(); }

bool HikCamera::open(int index, const std::string& targetSerial) {
    deviceIndex_ = index;

    MV_CC_DEVICE_INFO_LIST deviceList;
    memset(&deviceList, 0, sizeof(deviceList));
    int ret = MV_CC_EnumDevices(MV_GIGE_DEVICE | MV_USB_DEVICE, &deviceList);
    if(ret != MV_OK) { fprintf(stderr, "[海康相机] 枚举设备失败: 0x%x\n", ret); return false; }
    if(deviceList.nDeviceNum == 0) { fprintf(stderr, "[海康相机] 未找到任何设备\n"); return false; }

    // 如果指定了 serial，按 serial 查找设备；否则用 index
    int targetIndex = -1;
    if (!targetSerial.empty()) {
        for (unsigned int i = 0; i < deviceList.nDeviceNum; i++) {
            auto* info = deviceList.pDeviceInfo[i];
            if (!info) continue;
            std::string sn;
            if (info->nTLayerType == MV_GIGE_DEVICE)
                sn = std::string((char*)info->SpecialInfo.stGigEInfo.chSerialNumber);
            else
                sn = std::string((char*)info->SpecialInfo.stUsb3VInfo.chSerialNumber);
            if (sn == targetSerial) {
                targetIndex = (int)i;
                fprintf(stderr, "[海康相机] 按 serial '%s' 找到设备索引 %d\n", targetSerial.c_str(), i);
                break;
            }
        }
        if (targetIndex < 0) {
            fprintf(stderr, "[海康相机] 未找到 serial='%s' 的设备\n", targetSerial.c_str());
            return false;
        }
    } else {
        if ((unsigned int)index >= deviceList.nDeviceNum) {
            fprintf(stderr, "[海康相机] 设备索引 %d 超出范围 (共 %u 个设备)\n", index, deviceList.nDeviceNum);
            return false;
        }
        targetIndex = index;
    }

    fprintf(stderr, "[海康相机] 发现 %d 个设备，打开索引 %d\n", deviceList.nDeviceNum, targetIndex);

    ret = MV_CC_CreateHandle(&handle_, deviceList.pDeviceInfo[targetIndex]);
    if(ret != MV_OK) { fprintf(stderr, "[海康相机] 创建句柄失败: 0x%x\n", ret); return false; }

    ret = MV_CC_OpenDevice(handle_);
    if(ret != MV_OK) {
        fprintf(stderr, "[海康相机] 独占打开失败: 0x%x, 尝试抢占模式...\n", ret);
        ret = MV_CC_OpenDevice(handle_, MV_ACCESS_ExclusiveWithSwitch, 0);
        if(ret != MV_OK) {
            fprintf(stderr, "[海康相机] 抢占打开也失败: 0x%x\n", ret);
            MV_CC_DestroyHandle(handle_); handle_ = nullptr; return false;
        }
    }

    MV_CC_DEVICE_INFO* pInfo = deviceList.pDeviceInfo[targetIndex];
    if(pInfo->nTLayerType == MV_GIGE_DEVICE) {
        unsigned int ip = pInfo->SpecialInfo.stGigEInfo.nCurrentIp;
        fprintf(stderr, "[海康相机] GigE IP: %d.%d.%d.%d\n",
            (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
        serialNumber_ = std::string((char*)pInfo->SpecialInfo.stGigEInfo.chSerialNumber);
        deviceName_ = std::string((char*)pInfo->SpecialInfo.stGigEInfo.chUserDefinedName);
        if (deviceName_.empty()) deviceName_ = std::string((char*)pInfo->SpecialInfo.stGigEInfo.chModelName);
    } else {
        serialNumber_ = std::string((char*)pInfo->SpecialInfo.stUsb3VInfo.chSerialNumber);
        deviceName_ = std::string((char*)pInfo->SpecialInfo.stUsb3VInfo.chUserDefinedName);
        if (deviceName_.empty()) deviceName_ = std::string((char*)pInfo->SpecialInfo.stUsb3VInfo.chModelName);
    }
    fprintf(stderr, "[海康相机] 序列号: %s, 型号: %s\n", serialNumber_.c_str(), deviceName_.c_str());

    // 设置分辨率
    int targetWidth, targetHeight;
    MVCC_INTVALUE param;
    MV_CC_GetIntValue(handle_, "WidthMax", &param);
    int sensorMaxW = param.nCurValue;
    MV_CC_GetIntValue(handle_, "HeightMax", &param);
    int sensorMaxH = param.nCurValue;

    if (cfg_.maxWidth > 0 && cfg_.maxHeight > 0) {
        targetWidth = cfg_.maxWidth;
        targetHeight = cfg_.maxHeight;
    } else {
        targetWidth = sensorMaxW;
        targetHeight = sensorMaxH;
    }

    MV_CC_SetIntValue(handle_, "OffsetX", 0);
    MV_CC_SetIntValue(handle_, "OffsetY", 0);
    MV_CC_SetIntValue(handle_, "Width", targetWidth);
    MV_CC_SetIntValue(handle_, "Height", targetHeight);

    MV_CC_GetIntValue(handle_, "Width", &param);
    int actualW = param.nCurValue;
    MV_CC_GetIntValue(handle_, "Height", &param);
    int actualH = param.nCurValue;
    fprintf(stderr, "[海康相机] 传感器: %dx%d, 采集: %dx%d\n", sensorMaxW, sensorMaxH, actualW, actualH);

    MV_CC_SetEnumValue(handle_, "TriggerMode", MV_TRIGGER_MODE_OFF);
    if (cfg_.fps > 0) {
        int fpsRet = MV_CC_SetBoolValue(handle_, "AcquisitionFrameRateEnable", true);
        int rateRet = MV_CC_SetFloatValue(handle_, "AcquisitionFrameRate", (float)cfg_.fps);
        fprintf(stderr, "[HikCamera] frame rate set to %d fps (enable=0x%x, rate=0x%x)\n",
                cfg_.fps, fpsRet, rateRet);
    }
    MV_CC_SetImageNodeNum(handle_, 10);
    // 注册回调时传入 this->rawFrame_ 作为用户数据，每个实例独立
    MV_CC_RegisterImageCallBackEx(handle_, imageCallback, rawFrame_.get());

    ret = MV_CC_StartGrabbing(handle_);
    if(ret != MV_OK) {
        fprintf(stderr, "[海康相机] 开始取流失败: 0x%x\n", ret);
        MV_CC_CloseDevice(handle_); MV_CC_DestroyHandle(handle_); handle_ = nullptr; return false;
    }

    opened_ = true;
    fprintf(stderr, "[海康相机] 取流已启动 (index=%d, SN=%s)\n", index, serialNumber_.c_str());
    return true;
}

void HikCamera::close() {
    if(handle_) {
        MV_CC_StopGrabbing(handle_);
        MV_CC_CloseDevice(handle_);
        MV_CC_DestroyHandle(handle_);
        handle_ = nullptr;
    }
    opened_ = false;
}

bool HikCamera::isOpened() const { return opened_; }

cv::Mat HikCamera::read() {
    if(!handle_ || !opened_) return cv::Mat();

    std::vector<uint8_t> rawData;
    int w, h;
    unsigned int pt;
    size_t rawLen;

    {
        std::lock_guard<std::mutex> lock(rawFrame_->mutex);
        if(!rawFrame_->hasNew) return cv::Mat();
        rawFrame_->hasNew = false;
        rawData = rawFrame_->data;
        w = rawFrame_->width;
        h = rawFrame_->height;
        pt = rawFrame_->pixelType;
        rawLen = rawFrame_->frameLen;
    }

    if (pt != lastPixelType_) {
        fprintf(stderr, "[HikCamera] SN=%s pixelType=0x%x size=%dx%d len=%zu\n",
                serialNumber_.c_str(), pt, w, h, rawLen);
        lastPixelType_ = pt;
    }

    // SDK 像素转换 — 部分像素格式需要更大缓冲区
    unsigned int bgrBufSize = w * h * 4;
    std::vector<unsigned char> bgrBuf(bgrBufSize);

    MV_CC_PIXEL_CONVERT_PARAM_EX cvtParam;
    memset(&cvtParam, 0, sizeof(cvtParam));
    cvtParam.nWidth = w;
    cvtParam.nHeight = h;
    cvtParam.enSrcPixelType = (MvGvspPixelType)pt;
    cvtParam.pSrcData = rawData.data();
    cvtParam.nSrcDataLen = (unsigned int)rawLen;
    cvtParam.enDstPixelType = PixelType_Gvsp_BGR8_Packed;
    cvtParam.pDstBuffer = bgrBuf.data();
    cvtParam.nDstBufferSize = bgrBufSize;

    int ret = MV_CC_ConvertPixelTypeEx(handle_, &cvtParam);
    cv::Mat frame;
    if(ret != MV_OK) {
        // SDK 像素转换失败时，尝试用 OpenCV 的 Bayer 转换兜底。
        if (rawData.size() >= (size_t)(w * h)) {
            cv::Mat bayer(h, w, CV_8UC1, rawData.data());
            int cvtCode = -1;
            if (pt == (0x01000000 | (8 << 16) | 0x0008)) cvtCode = cv::COLOR_BayerGR2BGR;      // BayerGR8
            else if (pt == (0x01000000 | (8 << 16) | 0x0009)) cvtCode = cv::COLOR_BayerRG2BGR; // BayerRG8
            else if (pt == (0x01000000 | (8 << 16) | 0x000A)) cvtCode = cv::COLOR_BayerGB2BGR; // BayerGB8
            else if (pt == (0x01000000 | (8 << 16) | 0x000B)) cvtCode = cv::COLOR_BayerBG2BGR; // BayerBG8

            if (cvtCode >= 0) {
                cv::cvtColor(bayer, frame, cvtCode);
            }
        }
        if (frame.empty()) {
            static int failCount = 0;
            if (++failCount <= 3 || failCount % 100 == 0)
                fprintf(stderr, "[海康相机] 像素转换失败: 0x%x (pixelType=0x%x, %dx%d)\n", ret, pt, w, h);
            return cv::Mat();
        }
    } else {
        frame = cv::Mat(h, w, CV_8UC3, bgrBuf.data()).clone();
    }

    // 鱼眼去畸变（参数从 config 读取）
    static cv::Mat map1, map2;
    if (mapW_ != w || mapH_ != h || mapK1_ != cfg_.fisheyeK1 || mapK2_ != cfg_.fisheyeK2 || mapScale_ != cfg_.fisheyeScale) {
        mapW_ = w; mapH_ = h;
        mapK1_ = cfg_.fisheyeK1; mapK2_ = cfg_.fisheyeK2; mapScale_ = cfg_.fisheyeScale;
        double fx = w * 0.85, fy = h * 0.85;
        double cx = w / 2.0, cy = h / 2.0;
        cv::Mat K = (cv::Mat_<double>(3,3) << fx, 0, cx, 0, fy, cy, 0, 0, 1);
        cv::Mat D = (cv::Mat_<double>(1,4) << cfg_.fisheyeK1, cfg_.fisheyeK2, 0.0, 0.0);
        cv::Mat newK = K.clone();
        newK.at<double>(0,0) = fx * cfg_.fisheyeScale;
        newK.at<double>(1,1) = fy * cfg_.fisheyeScale;
        cv::fisheye::initUndistortRectifyMap(K, D, cv::Mat::eye(3,3,CV_64F), newK, cv::Size(w,h), CV_16SC2, map1_, map2_);
        fprintf(stderr, "[海康相机] 鱼眼校正: K1=%.2f K2=%.2f scale=%.2f\n", cfg_.fisheyeK1, cfg_.fisheyeK2, cfg_.fisheyeScale);
    }
    cv::remap(frame, frame, map1_, map2_, cv::INTER_LINEAR);

    // 亮度和对比度
    if (cfg_.contrast != 1.0 || cfg_.brightness != 0.0) {
        frame.convertTo(frame, -1, cfg_.contrast, cfg_.brightness);
    }

    return frame;
}

#else // NO_HIK_CAMERA - stub

struct HikCamera::RawFrameBuffer {};

HikCamera::HikCamera(const CameraConfig& cfg) : handle_(nullptr), opened_(false), cfg_(cfg) {
    rawFrame_ = std::make_unique<RawFrameBuffer>();
}
HikCamera::~HikCamera() { close(); }
bool HikCamera::open(int, const std::string&) { fprintf(stderr, "[海康相机] SDK 未安装\n"); return false; }
void HikCamera::close() { opened_ = false; }
bool HikCamera::isOpened() const { return false; }
cv::Mat HikCamera::read() { return cv::Mat(); }

#endif
