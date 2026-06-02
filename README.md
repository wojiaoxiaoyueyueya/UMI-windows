# 机器人数据采集平台

这是一个本地运行的机器人数据采集系统，包含 C++ 后端服务、Web 前端、相机/夹爪设备接入、数据录制、历史数据管理，以及 LeRobot/HDF5/RLDS 数据转换脚本。

当前主版本面向 **Windows 10/11 x64**。工程里保留了部分 Linux SDK 目录，但现有串口、GCAN、运行时 DLL 和部分设备逻辑仍以 Windows 为主，Linux 版本需要后续单独适配设备驱动层。

## 功能概览

- 多相机预览：海康 MVS 工业相机、Orbbec Gemini 305。
- Orbbec 数据：彩色、深度、红外、点云。
- UMI 手动夹爪：位置、按钮、LED、录制联动。
- GCAN 电动夹爪：使能、位置、速度、电流、MIT、急停。
- 剪刀石头布：浏览器 UVC 相机识别手势，联动电动夹爪出拳。
- 数据采集：视频、点云、夹爪 CSV、统一时间戳、metadata。
- 数据转换：LeRobot、HDF5、RLDS/TFRecord。
- 数据管理：历史会话查看、删除、转换状态查看。

## 推荐目录结构

仓库应保留这些目录和文件：

```text
ManualGripper/
├─ CMakeLists.txt
├─ config.json
├─ requirements.txt
├─ collect_dlls.ps1
├─ setup_orbbec_sdk.ps1
├─ README.md
├─ docs/
├─ frontend/
├─ include/
├─ src/
├─ tools/
└─ lib/
   ├─ gcan/
   ├─ hikvision/
   ├─ orbbec/
   └─ umi/
```

我没有提交到 Git 的内容：

- `build/`：构建产物、exe、DLL、日志，可重新生成。
- `data_capture/`：采集得到的原始数据，通常很大。
- `data_converted/`：转换后的训练数据，通常很大。
- `__pycache__/`、`*.pyc`、`*.log`、临时输出文件。

## 依赖说明

### 必需软件

1. Windows 10/11 x64。
2. MSYS2，推荐安装在 `C:\msys64`。
3. MinGW64 工具链、CMake、OpenCV、Eigen3。
4. Python 3 和转换脚本依赖。

MSYS2 MINGW64 终端里安装 C++ 依赖：

```bash
pacman -Syu
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-make mingw-w64-x86_64-opencv mingw-w64-x86_64-eigen3 mingw-w64-x86_64-python
```

安装 Python 依赖：

```bash
python -m pip install --upgrade pip
python -m pip install -r requirements.txt
```

### 硬件 SDK

工程按下面路径查找 SDK：

```text
lib/hikvision/include
lib/hikvision/lib/win64/MvCameraControl.lib
lib/orbbec/include
lib/orbbec/lib/win64/OrbbecSDK.dll
lib/gcan/ECanVci64.dll
lib/gcan/CHUSBDLL64.dll
lib/umi/
```

如果仓库里已经带了这些 SDK 文件，别人拉取后只需要安装对应厂商驱动即可。  
如果因为授权或体积原因不提交 SDK，请保留目录结构，并让使用者从厂商 SDK 复制文件到上述位置。

## 从零部署

### 1. 拉取项目

```bash
git clone <你的仓库地址> ManualGripper
cd ManualGripper
```

建议把工程放在英文路径，例如：

```text
C:\Robot\ManualGripper
```

### 2. 安装硬件驱动

- 海康相机：安装 Hikvision MVS 客户端和驱动。
- Orbbec 相机：安装 Orbbec Viewer/SDK/驱动。
- UMI 手动夹爪：插入后确认设备管理器能看到 COM 口。
- GCAN 电动夹爪：安装 USBCAN 驱动，确认 CAN 线、电源、终端电阻和电机 ID。
- IMX335：作为 UVC 摄像头使用，先用 Windows 相机 App 确认能打开。

### 3. 检查配置

默认配置在 `config.json`：

```json
{
  "server": { "port": 8080 },
  "paths": {
    "frontendDir": "frontend",
    "dataDir": "data_capture",
    "convertOutputDir": "data_converted",
    "convertScript": "tools/convert_to_lerobot.py"
  }
}
```

常用调整：

- 端口冲突：修改 `server.port`。
- 数据盘空间不足：修改 `paths.dataDir` 和 `paths.convertOutputDir`。
- 三路海康卡顿：降低 `camera.maxWidth`、`camera.maxHeight` 或 `camera.fps`。
- 预览压力大：降低 `stream.streamMaxWidth` 或 `stream.jpegQuality`。

### 4. 编译

MSYS2 MINGW64 终端：

```bash
cd /c/Robot/ManualGripper
mkdir -p build
cd build
cmake .. -G "MinGW Makefiles"
mingw32-make -j4
```

PowerShell：

```powershell
cd C:\Robot\ManualGripper
$env:PATH = "C:\msys64\mingw64\bin;" + $env:PATH
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build --config Release
```

如果提示缺 DLL，运行：

```powershell
.\collect_dlls.ps1
```

### 5. 启动

```powershell
cd C:\Robot\ManualGripper\build
.\ManualGripper.exe
```

浏览器打开：

```text
http://localhost:8080
```

常用页面：

- 数据看板：`http://localhost:8080/index.html`
- 采集控制台：`http://localhost:8080/index_old.html`
- 项目说明：`http://localhost:8080/info.html`

## 首次运行检查

1. 启动 `ManualGripper.exe`。
2. 打开 `http://localhost:8080/index_old.html`。
3. 点击“重新扫描设备”。
4. 右上角设备信息中确认相机和夹爪数量。
5. 左侧打开需要的相机流或夹爪流。
6. 海康只应显示彩色流；Orbbec 应显示彩色、深度、红外、点云。
7. 在数据采集页录制 5 秒测试数据。
8. 停止录制后确认 `data_capture/` 下生成新会话。
9. 进入数据转换页，把测试会话转换为 LeRobot 或 HDF5。
10. 回到数据看板确认历史记录可查看。

## 数据格式

原始数据目录：

```text
data_capture/20260602_103000/
├─ Head-umi/
│  ├─ color_video/
│  ├─ depth_video/
│  ├─ ir-left_video/
│  ├─ ir-right_video/
│  └─ pointcloud_data/
├─ Left-umi/
│  ├─ color_video/
│  └─ gripper_data/
├─ Right-umi/
│  ├─ color_video/
│  └─ gripper_data/
└─ metadata.json
```

视频目录包含：

- `*.mp4`：视频文件。
- `timestamps.csv`：每帧时间戳。

夹爪目录包含：

- `gripper.csv`：位置、按钮、电动夹爪状态或 CAN 数据。

转换输出目录：

```text
data_converted/
├─ 20260602_103000_lerobot/
├─ 20260602_103000_hdf5/
└─ 20260602_103000_rlds/
```

## Git 提交和仓库维护

### 第一次创建仓库

在项目根目录，也就是包含 `CMakeLists.txt` 的目录执行：

```bash
git init
git status
git add .gitattributes .gitignore README.md requirements.txt CMakeLists.txt config.json collect_dlls.ps1 setup_orbbec_sdk.ps1 docs frontend include src tools lib
git commit -m "Initial data capture platform"
```

连接远程仓库：

```bash
git remote add origin <你的仓库地址>
git branch -M main
git push -u origin main
```

### 后续提交代码

```bash
git status
git add README.md docs frontend include src tools config.json requirements.txt .gitignore .gitattributes
git commit -m "Update device hotplug and deployment docs"
git push
```

### 别人下载或拉取

第一次：

```bash
git clone <你的仓库地址> ManualGripper
cd ManualGripper
python -m pip install -r requirements.txt
```

后续更新：

```bash
cd ManualGripper
git pull
cmake --build build --config Release
```

### 提交前检查

每次提交前建议执行：

```bash
git status
git diff --stat
cmake --build build --config Release
```

如果看到 `build/`、`data_capture/`、`data_converted/` 准备被提交，说明 `.gitignore` 或 `git add` 范围有问题，不要提交这些目录。

## 常见问题

### 页面打不开

确认服务是否启动，默认端口是 `8080`。端口被占用时修改 `config.json` 的 `server.port`。

### 相机能在右上角设备信息中看到，但左侧没有开关

右上角显示的是检测列表，左侧显示的是已挂载到采集槽位的设备。点击“重新扫描设备”，让后端释放旧槽位并重新挂载设备。

### 海康三路画面卡顿

检查 `config.json` 中的：

```json
"camera": {
  "maxWidth": 1280,
  "maxHeight": 720,
  "fps": 30
}
```

三路 USB 相机不建议满分辨率满帧率长期预览。

### 海康颜色异常

优先用 MVS 客户端确认相机输出正常。项目里默认使用海康 SDK 转换为 BGR 图像。

### Orbbec 黑屏

先用 Orbbec Viewer 验证设备，再回到网页点击“重新扫描设备”。如果刚切换过海康和 Orbbec，建议关闭旧预览流后再扫描。

### 夹爪无反应

检查设备管理器 COM 口、电源、线缆、CAN 适配器、使能状态、急停状态和错误码。电动夹爪首次测试不要直接发送大行程。

## 版本整理建议

为了便于迁移，建议把项目分成两类：

- 仓库源码版：提交源码、前端、文档、配置、工具脚本和必要 SDK 目录，不提交数据和 build。
- 现场运行版：在源码版基础上保留 `build/`、`data_capture/`、`data_converted/`，用于当前电脑直接运行和保留历史数据。

如果要给别人使用，优先发仓库源码版；如果对方不会编译，再额外发一个压缩包运行版。
