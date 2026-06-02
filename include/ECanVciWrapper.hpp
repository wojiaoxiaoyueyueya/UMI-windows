// ECanVciWrapper.hpp - 广成科技 ECanVci64.dll 动态加载封装
// 通过 LoadLibrary 动态加载 CAN SDK，无需链接 .lib 文件

#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include <mutex>
#include <cstdio>

// ---- CAN SDK 结构体定义（来自 ECanVci.h） ----
// 注意：不使用 pack(1)，与 SDK 原始头文件保持一致（默认对齐）

typedef struct _ECAN_BOARD_INFO {
    USHORT hw_Version;
    USHORT fw_Version;
    USHORT dr_Version;
    USHORT in_Version;
    USHORT irq_Num;
    BYTE   can_Num;
    CHAR   str_Serial_Num[20];
    CHAR   str_hw_Type[40];
    USHORT Reserved[4];
} ECAN_BOARD_INFO;

typedef struct _ECAN_CAN_OBJ {
    UINT  ID;
    UINT  TimeStamp;
    BYTE  TimeFlag;
    BYTE  SendType;
    BYTE  RemoteFlag;
    BYTE  ExternFlag;
    BYTE  DataLen;
    BYTE  Data[8];
    BYTE  Reserved[3];
} ECAN_CAN_OBJ;

typedef struct _ECAN_INIT_CONFIG {
    DWORD AccCode;
    DWORD AccMask;
    DWORD Reserved;
    UCHAR Filter;
    UCHAR Timing0;
    UCHAR Timing1;
    UCHAR Mode;
} ECAN_INIT_CONFIG;

// ---- API 函数指针类型 ----
typedef DWORD (__stdcall *FnOpenDevice)(DWORD, DWORD, DWORD);
typedef DWORD (__stdcall *FnCloseDevice)(DWORD, DWORD);
typedef DWORD (__stdcall *FnInitCAN)(DWORD, DWORD, DWORD, ECAN_INIT_CONFIG*);
typedef DWORD (__stdcall *FnStartCAN)(DWORD, DWORD, DWORD);
typedef DWORD (__stdcall *FnResetCAN)(DWORD, DWORD, DWORD);
typedef ULONG (__stdcall *FnTransmit)(DWORD, DWORD, DWORD, ECAN_CAN_OBJ*, ULONG);
typedef ULONG (__stdcall *FnReceive)(DWORD, DWORD, DWORD, ECAN_CAN_OBJ*, ULONG, INT);
typedef DWORD (__stdcall *FnReadBoardInfo)(DWORD, DWORD, ECAN_BOARD_INFO*);
typedef ULONG (__stdcall *FnGetReceiveNum)(DWORD, DWORD, DWORD);
typedef DWORD (__stdcall *FnClearBuffer)(DWORD, DWORD, DWORD);
typedef DWORD (__stdcall *FnReadErrInfo)(DWORD, DWORD, DWORD, void*);

class ECanVciWrapper {
public:
    ECanVciWrapper() : hModule_(nullptr), opened_(false) {}
    ~ECanVciWrapper() { close(); unload(); }

    bool load(const std::string& searchDir = "") {
        if (hModule_) return true;

        std::vector<std::string> paths;
        if (!searchDir.empty()) {
            paths.push_back(searchDir + "\\ECanVci64.dll");
            paths.push_back(searchDir + "\\ECANVCI库文件64位\\ECanVci64.dll");
        }
        paths.push_back("ECanVci64.dll");
        paths.push_back("ECanVci.dll");

        for (const auto& p : paths) {
            hModule_ = LoadLibraryA(p.c_str());
            if (hModule_) {
                fprintf(stderr, "[CAN] 已加载 DLL: %s\n", p.c_str());
                break;
            }
        }

        if (!hModule_) {
            fprintf(stderr, "[CAN] 未找到 ECanVci64.dll\n");
            return false;
        }

        #define LOAD_FN(name) \
            fn_##name = (Fn##name)GetProcAddress(hModule_, #name); \
            if (!fn_##name) { fprintf(stderr, "[CAN] 未找到函数: %s\n", #name); return false; }

        LOAD_FN(OpenDevice);
        LOAD_FN(CloseDevice);
        LOAD_FN(InitCAN);
        LOAD_FN(StartCAN);
        LOAD_FN(ResetCAN);
        LOAD_FN(Transmit);
        LOAD_FN(Receive);
        LOAD_FN(ReadBoardInfo);
        LOAD_FN(GetReceiveNum);
        LOAD_FN(ClearBuffer);
        LOAD_FN(ReadErrInfo);
        #undef LOAD_FN

        return true;
    }

    void unload() {
        if (hModule_) {
            FreeLibrary(hModule_);
            hModule_ = nullptr;
        }
    }

    bool openDevice(DWORD deviceType, DWORD deviceIndex = 0) {
        if (!fn_OpenDevice) return false;
        DWORD ret = fn_OpenDevice(deviceType, deviceIndex, 0);
        fprintf(stderr, "[CAN] OpenDevice ret=%lu (期望1)\n", ret);
        if (ret == 1) { // STATUS_OK = 1
            deviceType_ = deviceType;
            deviceIndex_ = deviceIndex;
            opened_ = true;
            return true;
        }
        fprintf(stderr, "[CAN] OpenDevice 失败, ret=%lu\n", ret);
        return false;
    }

    bool initCAN(DWORD channel, BYTE timing0 = 0x00, BYTE timing1 = 0x14) { // 默认1Mbps
        if (!fn_InitCAN || !opened_) return false;
        ECAN_INIT_CONFIG cfg = {};
        cfg.AccCode = 0x00000000;
        cfg.AccMask = 0xFFFFFFFF;
        cfg.Filter = 0;
        cfg.Timing0 = timing0;
        cfg.Timing1 = timing1;
        cfg.Mode = 0; // Normal mode
        DWORD ret = fn_InitCAN(deviceType_, deviceIndex_, channel, &cfg);
        if (ret != 1) {
            fprintf(stderr, "[CAN] InitCAN 失败, ret=%lu\n", ret);
            return false;
        }
        channel_ = channel;
        return true;
    }

    bool startCAN() {
        if (!fn_StartCAN || !opened_) return false;
        DWORD ret = fn_StartCAN(deviceType_, deviceIndex_, channel_);
        if (ret != 1) {
            fprintf(stderr, "[CAN] StartCAN 失败, ret=%lu\n", ret);
            return false;
        }
        // 启动后清除缓冲区，避免读到残留数据
        if (fn_ClearBuffer) {
            fn_ClearBuffer(deviceType_, deviceIndex_, channel_);
        }
        return true;
    }

    bool transmit(const ECAN_CAN_OBJ& obj) {
        if (!fn_Transmit || !opened_) {
            fprintf(stderr, "[CAN] transmit 失败: fn_Transmit=%p opened=%d\n",
                    (void*)fn_Transmit, opened_);
            return false;
        }
        std::lock_guard<std::mutex> lock(txMutex_);
        ULONG ret = fn_Transmit(deviceType_, deviceIndex_, channel_,
                                 const_cast<ECAN_CAN_OBJ*>(&obj), 1);
        if (ret == 0) {
            // 发送失败，读取错误并尝试复位重发
            UINT errCode = readError();
            printError(errCode);
            // 如果是总线错误(bus-off)，复位 CAN 后重试一次
            if (errCode & 0x0080) { // bus-off bit
                fprintf(stderr, "[CAN] 检测到 bus-off，尝试复位...\n");
                resetCAN();
                Sleep(50);
                ret = fn_Transmit(deviceType_, deviceIndex_, channel_,
                                   const_cast<ECAN_CAN_OBJ*>(&obj), 1);
                if (ret > 0) {
                    fprintf(stderr, "[CAN] 复位后重发成功, ID=0x%03X\n", obj.ID);
                    return true;
                }
            }
        }
        return ret > 0;
    }

    bool resetCAN() {
        if (!fn_ResetCAN || !opened_) return false;
        DWORD ret = fn_ResetCAN(deviceType_, deviceIndex_, channel_);
        if (ret == 1) {
            // 复位后重新初始化并启动
            ECAN_INIT_CONFIG cfg = {};
            cfg.AccCode = 0x00000000;
            cfg.AccMask = 0xFFFFFFFF;
            cfg.Filter = 0;
            cfg.Timing0 = 0x00;
            cfg.Timing1 = 0x14;
            cfg.Mode = 0;
            fn_InitCAN(deviceType_, deviceIndex_, channel_, &cfg);
            fn_StartCAN(deviceType_, deviceIndex_, channel_);
            if (fn_ClearBuffer) fn_ClearBuffer(deviceType_, deviceIndex_, channel_);
        }
        return ret == 1;
    }

    UINT readError() {
        if (!fn_ReadErrInfo || !opened_) return 0;
        BYTE errBuf[8] = {};
        DWORD errRet = fn_ReadErrInfo(deviceType_, deviceIndex_, channel_, errBuf);
        if (errRet == 1) {
            UINT errCode = 0;
            memcpy(&errCode, errBuf, 4);
            return errCode;
        }
        return 0;
    }

    void printError(UINT errCode) {
        fprintf(stderr, "[CAN] 错误信息: ErrCode=0x%04X", errCode);
        if (errCode & 0x0001) fprintf(stderr, " [FIFO溢出]");
        if (errCode & 0x0002) fprintf(stderr, " [错误报警]");
        if (errCode & 0x0004) fprintf(stderr, " [被动错误]");
        if (errCode & 0x0008) fprintf(stderr, " [仲裁丢失]");
        if (errCode & 0x0010) fprintf(stderr, " [总线错误]");
        if (errCode & 0x0080) fprintf(stderr, " [bus-off]");
        if (errCode & 0x0100) fprintf(stderr, " [设备已打开]");
        if (errCode & 0x0200) fprintf(stderr, " [打开设备失败]");
        if (errCode & 0x0400) fprintf(stderr, " [设备未打开]");
        if (errCode & 0x1000) fprintf(stderr, " [设备不存在]");
        if (errCode & 0x4000) fprintf(stderr, " [命令执行失败]");
        fprintf(stderr, "\n");
    }

    int receive(ECAN_CAN_OBJ* buf, int maxCount, int waitTime = 10) {
        if (!fn_Receive || !opened_) return 0;
        return (int)fn_Receive(deviceType_, deviceIndex_, channel_, buf, maxCount, waitTime);
    }

    ULONG getReceiveNum() {
        if (!fn_GetReceiveNum || !opened_) return 0;
        return fn_GetReceiveNum(deviceType_, deviceIndex_, channel_);
    }

    bool readBoardInfo(ECAN_BOARD_INFO& info) {
        if (!fn_ReadBoardInfo || !opened_) return false;
        return fn_ReadBoardInfo(deviceType_, deviceIndex_, &info) == 1;
    }

    void close() {
        if (opened_ && fn_CloseDevice) {
            fn_CloseDevice(deviceType_, deviceIndex_);
            opened_ = false;
        }
    }

    bool isOpened() const { return opened_; }

    static ECanVciWrapper& sharedInstance() {
        static ECanVciWrapper instance;
        return instance;
    }

private:
    HMODULE hModule_;
    bool opened_;
    DWORD deviceType_ = 4; // USBCAN2
    DWORD deviceIndex_ = 0;
    DWORD channel_ = 0;
    std::mutex txMutex_;

    FnOpenDevice    fn_OpenDevice = nullptr;
    FnCloseDevice   fn_CloseDevice = nullptr;
    FnInitCAN       fn_InitCAN = nullptr;
    FnStartCAN      fn_StartCAN = nullptr;
    FnResetCAN      fn_ResetCAN = nullptr;
    FnTransmit      fn_Transmit = nullptr;
    FnReceive       fn_Receive = nullptr;
    FnReadBoardInfo fn_ReadBoardInfo = nullptr;
    FnGetReceiveNum fn_GetReceiveNum = nullptr;
    FnClearBuffer   fn_ClearBuffer = nullptr;
    FnReadErrInfo   fn_ReadErrInfo = nullptr;
};
