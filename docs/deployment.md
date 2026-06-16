# 部署、迁移和 Git 手册

本文档用于把项目交给新电脑或其他同事时使用。

## 1. 仓库应包含什么

必须包含：

```text
CMakeLists.txt
config.json
requirements.txt
README.md
docs/
frontend/
include/
src/
tools/
lib/
collect_dlls.ps1
setup_orbbec_sdk.ps1
```

不应提交：

```text
build/
data_capture/
data_converted/
__pycache__/
*.log
*.pyc
```

## 2. 新电脑部署步骤

1. 安装 Windows 10/11 x64。
2. 安装 MSYS2 到 `C:\msys64`。
3. 打开 MSYS2 MINGW64，安装编译依赖：

```bash
pacman -Syu
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-make mingw-w64-x86_64-opencv mingw-w64-x86_64-eigen3 mingw-w64-x86_64-python
```

4. 克隆仓库：

```bash
git clone <仓库地址> ManualGripper
cd ManualGripper
```

5. 安装 Python 依赖：

```bash
python -m pip install -r requirements.txt
```

6. 安装硬件驱动：

- 海康：MVS 客户端和驱动。
- Orbbec：Orbbec SDK/Viewer/驱动。
- UMI：USB 串口驱动。
- GCAN：USBCAN 驱动。
- ESP32-CAN：烧录 `can_transceiver` 固件，并安装对应 USB 串口驱动。

7. 检查 SDK 目录：

```text
lib/hikvision/include
lib/hikvision/lib/win64/MvCameraControl.lib
lib/orbbec/include
lib/orbbec/lib/win64/OrbbecSDK.dll
lib/gcan/ECanVci64.dll
lib/gcan/CHUSBDLL64.dll
```

8. 编译：

```bash
mkdir -p build
cd build
cmake .. -G "MinGW Makefiles"
mingw32-make -j4
```

9. 启动：

```bash
./ManualGripper.exe
```

10. 浏览器访问：

```text
http://localhost:8080
```

## 3. Git 首次提交

```bash
git init
git status
git add .gitattributes .gitignore README.md requirements.txt CMakeLists.txt config.json collect_dlls.ps1 setup_orbbec_sdk.ps1 docs frontend include src tools lib
git commit -m "Initial data capture platform"
git remote add origin <仓库地址>
git branch -M main
git push -u origin main
```

## 4. Git 后续提交

```bash
git status
git diff --stat
git add README.md docs frontend include src tools config.json requirements.txt .gitignore .gitattributes
git commit -m "Update platform version"
git push
```

## 5. 别人拉取更新

```bash
git pull
cmake --build build --config Release
```

如果是第一次拉取，则先执行：

```bash
python -m pip install -r requirements.txt
```

## 6. 提交前检查清单

1. `git status` 中不应出现 `build/`。
2. `git status` 中不应出现 `data_capture/`。
3. `git status` 中不应出现 `data_converted/`。
4. 编译应通过：

```bash
cmake --build build --config Release
```

5. 启动后能打开：

```text
http://localhost:8080
http://localhost:8080/index_old.html
http://localhost:8080/info.html
```

## 7. 运行版压缩包

如果对方不会编译，可以在已编译电脑上制作运行版压缩包，包含：

```text
build/
frontend/
config.json
tools/
README.md
requirements.txt
```

运行版仍要求新电脑安装硬件驱动。采集数据和转换数据是否打包，按实际需要决定。
