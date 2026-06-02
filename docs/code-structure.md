# 代码结构说明

本文档说明工程中各目录和核心文件的职责，便于后续维护、移植和排查问题。

## 后端源码

| 路径 | 职责 |
| --- | --- |
| `src/main.cpp` | 程序入口，加载配置，初始化设备，启动相机线程、夹爪线程、热插拔检查线程和 HTTP 服务。 |
| `src/Config.cpp` | 读取 `config.json`，生成运行配置。 |
| `src/DeviceManager.cpp` | 设备扫描、相机槽位分配、夹爪检测、设备状态 JSON 输出。 |
| `src/HikCamera.cpp` | 海康 MVS 相机封装，负责打开设备、取帧、像素转换、鱼眼矫正。 |
| `src/OrbbecCamera.cpp` | Orbbec 相机封装，负责彩色、深度、红外、点云回调。 |
| `src/UmiGripper.cpp` | UMI 手动夹爪串口通信、按钮、位置和 LED 控制。 |
| `src/ElectricGripper.cpp` | GCAN 电动夹爪通信、位置/速度/电流控制和状态解析。 |
| `src/SlamManager.cpp` | 可选 SLAM/位姿记录模块。 |
| `src/http/HttpServer.cpp` | HTTP 服务生命周期、MJPEG 编码循环、实时状态更新。 |
| `src/http/HttpServerRoutes.cpp` | REST API、静态页面、MJPEG 流、设备控制接口。 |
| `src/http/HttpServerRecording.cpp` | 录制、保存、时间戳、metadata、异步收尾和数据转换调度。 |
| `src/utils/JsonHelper.cpp` | JSON 响应等通用工具。 |

## 头文件

| 路径 | 职责 |
| --- | --- |
| `include/ICamera.hpp` | 相机统一接口，抽象海康和 Orbbec。 |
| `include/IGripper.hpp` | 夹爪统一接口。 |
| `include/HttpServer.hpp` | HTTP 服务、流状态、录制状态、转换状态的数据结构。 |
| `include/DeviceManager.hpp` | 设备管理器接口和槽位结构。 |
| `include/*Camera.hpp` | 具体相机类声明。 |
| `include/*Gripper.hpp` | 具体夹爪类声明。 |
| `include/utils/` | 通用工具声明。 |

## 前端

| 路径 | 职责 |
| --- | --- |
| `frontend/index.html` | 数据看板入口。 |
| `frontend/dashboard.js` | 数据看板逻辑，读取历史会话、转换结果、删除数据。 |
| `frontend/dashboard.css` | 数据看板样式。 |
| `frontend/index_old.html` | 采集控制台入口。 |
| `frontend/script.js` | 采集控制台逻辑，包括设备状态、视频流、录制、转换、夹爪控制、剪刀石头布。 |
| `frontend/style.css` | 采集控制台样式。 |
| `frontend/info.html` | 项目说明和部署手册页面。 |

## 工具脚本

| 路径 | 职责 |
| --- | --- |
| `tools/convert_to_lerobot.py` | 原始会话转换为 LeRobot 数据集。 |
| `tools/convert_to_hdf5.py` | 原始会话转换为 HDF5 数据集。 |
| `tools/convert_to_rlds.py` | 原始会话转换为 RLDS/TFRecord 风格数据。 |
| `collect_dlls.ps1` | 构建后补充收集运行 DLL。 |
| `setup_orbbec_sdk.ps1` | Orbbec SDK 放置辅助脚本。 |

## 第三方依赖目录

| 路径 | 内容 |
| --- | --- |
| `lib/hikvision/` | 海康 MVS SDK 头文件、库和运行 DLL。 |
| `lib/orbbec/` | Orbbec SDK 头文件、库和运行 DLL。 |
| `lib/gcan/` | GCAN USBCAN 头文件和 DLL。 |
| `lib/umi/` | UMI 夹爪驱动源码或兼容接口。 |

## 数据目录

| 路径 | 内容 | Git 策略 |
| --- | --- | --- |
| `data_capture/` | 原始采集会话。 | 不提交。 |
| `data_converted/` | 转换后的训练数据。 | 不提交。 |
| `build/` | exe、DLL、构建缓存和日志。 | 不提交。 |

## 维护规则

1. 新增 HTTP API 放在 `src/http/HttpServerRoutes.cpp`。
2. 新增录制、保存、时间戳、metadata 或转换逻辑放在 `src/http/HttpServerRecording.cpp`。
3. 新增实时状态更新、MJPEG 编码、流状态处理放在 `src/http/HttpServer.cpp`。
4. 新增设备类型时，优先实现 `ICamera` 或 `IGripper` 的适配类，再交给 `DeviceManager` 分配槽位。
5. 前端新增页面状态时，避免直接散落全局变量，优先复用已有状态缓存和渲染函数。
6. 不在 `build/` 中修改源码；所有源码修改都应发生在 `src/`、`include/`、`frontend/`、`tools/` 或 `docs/`。
