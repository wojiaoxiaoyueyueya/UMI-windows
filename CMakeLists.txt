# CMakeLists.txt - Windows 版数据采集项目
# 依赖：OpenCV (MSYS2 MinGW)、cpp-httplib、海康 MVS SDK（可选）、OrbbecSDK v2（可选）
# 编译：MinGW-w64 (MSYS2)

cmake_minimum_required(VERSION 3.10)
project(ManualGripper)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows 10 API 级别
add_compile_definitions(_WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN)

# ---- OpenCV (MSYS2 MinGW 版) ----
if(NOT DEFINED ENV{MSYS2_ROOT})
    if(EXISTS "C:/msys64/mingw64")
        set(MSYS2_ROOT "C:/msys64/mingw64")
    else()
        set(MSYS2_ROOT "/mingw64")
    endif()
else()
    set(MSYS2_ROOT "$ENV{MSYS2_ROOT}")
endif()

set(OpenCV_DIR "${MSYS2_ROOT}/lib/cmake/opencv4")
find_package(OpenCV REQUIRED)

# ---- Eigen3（SLAM 模块需要） ----
find_package(Eigen3 QUIET)
if(NOT Eigen3_FOUND)
    # 尝试 MSYS2 路径
    if(EXISTS "${MSYS2_ROOT}/include/eigen3/Eigen/Core")
        set(EIGEN3_INCLUDE_DIR "${MSYS2_ROOT}/include/eigen3")
        set(Eigen3_FOUND TRUE)
        message(STATUS "Eigen3: FOUND at ${EIGEN3_INCLUDE_DIR}")
    else()
        message(WARNING "Eigen3 not found. SLAM module will not be available. "
                         "Install via: pacman -S mingw-w64-x86_64-eigen3")
    endif()
endif()

# ---- 海康 MVS SDK（可选） ----
set(HIK_SDK_PATH ${CMAKE_SOURCE_DIR}/lib/hikvision)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(HIK_LIB "${HIK_SDK_PATH}/lib/win64/MvCameraControl.lib")
else()
    set(HIK_LIB "${HIK_SDK_PATH}/lib/win32/MvCameraControl.lib")
endif()

if(EXISTS "${HIK_LIB}")
    set(HIK_SDK_FOUND TRUE)
    message(STATUS "Hikvision SDK: FOUND")
else()
    set(HIK_SDK_FOUND FALSE)
    message(WARNING "Hikvision SDK not found. Building WITHOUT Hikvision camera support.")
endif()

# ---- OrbbecSDK v2（可选） ----
set(ORBBEC_SDK_PATH ${CMAKE_SOURCE_DIR}/lib/orbbec)
set(ORBBEC_INCLUDE_PATH "${ORBBEC_SDK_PATH}/include")

# 检查头文件是否存在（不需要 .lib，MinGW 用 LoadLibrary 动态加载）
if(EXISTS "${ORBBEC_INCLUDE_PATH}/libobsensor/h/ObSensor.h" OR EXISTS "${ORBBEC_INCLUDE_PATH}/libobsensor/ObSensor.h")
    set(ORBBEC_SDK_FOUND TRUE)
    message(STATUS "OrbbecSDK: FOUND (headers at ${ORBBEC_INCLUDE_PATH})")
else()
    set(ORBBEC_SDK_FOUND FALSE)
    message(WARNING "OrbbecSDK not found. Building WITHOUT Orbbec camera support. "
                     "Place SDK headers in lib/orbbec/include/")
endif()

# ---- 源文件 ----
set(SOURCES
    src/main.cpp
    src/Config.cpp
    src/http/HttpServer.cpp
    src/http/HttpServerRoutes.cpp
    src/http/HttpServerRecording.cpp
    src/UmiGripper.cpp
    src/ElectricGripper.cpp
    src/HikCamera.cpp
    src/DeviceManager.cpp
    src/OrbbecCamera.cpp
    src/SlamManager.cpp
    src/utils/JsonHelper.cpp
)

add_executable(ManualGripper ${SOURCES})

target_include_directories(ManualGripper PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${HIK_SDK_PATH}/include
    ${OpenCV_INCLUDE_DIRS}
)

# Eigen3（SLAM 模块）
if(Eigen3_FOUND AND NOT DEFINED EIGEN3_INCLUDE_DIR)
    target_link_libraries(ManualGripper PRIVATE Eigen3::Eigen)
elseif(DEFINED EIGEN3_INCLUDE_DIR)
    target_include_directories(ManualGripper PRIVATE ${EIGEN3_INCLUDE_DIR})
endif()

# Orbbec SDK 头文件
if(ORBBEC_SDK_FOUND)
    target_include_directories(ManualGripper PRIVATE ${ORBBEC_INCLUDE_PATH})
    # 检查是否有 .lib 文件可用于链接
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(ORBBEC_LIB_DIR "${ORBBEC_SDK_PATH}/lib/win64")
    else()
        set(ORBBEC_LIB_DIR "${ORBBEC_SDK_PATH}/lib/win32")
    endif()
    if(EXISTS "${ORBBEC_LIB_DIR}/OrbbecSDK.lib")
        target_link_directories(ManualGripper PRIVATE ${ORBBEC_LIB_DIR})
        target_link_libraries(ManualGripper PRIVATE OrbbecSDK)
    else()
        # 无 .lib 文件时使用动态加载模式
        message(STATUS "OrbbecSDK .lib not found, using dynamic loading")
    endif()
else()
    target_compile_definitions(ManualGripper PRIVATE NO_ORBBEC_CAMERA)
endif()

# 海康 SDK
if(HIK_SDK_FOUND)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        target_link_directories(ManualGripper PRIVATE ${HIK_SDK_PATH}/lib/win64)
    else()
        target_link_directories(ManualGripper PRIVATE ${HIK_SDK_PATH}/lib/win32)
    endif()
    target_link_libraries(ManualGripper PRIVATE MvCameraControl)
else()
    target_compile_definitions(ManualGripper PRIVATE NO_HIK_CAMERA)
endif()

target_link_libraries(ManualGripper PRIVATE
    ${OpenCV_LIBS}
    ws2_32
    setupapi
    shlwapi
)

# ---- 构建后复制运行时 DLL ----
if(HIK_SDK_FOUND)
    add_custom_command(TARGET ManualGripper POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${HIK_SDK_PATH}/lib/win64"
            "$<TARGET_FILE_DIR:ManualGripper>"
        COMMENT "Copying Hikvision SDK DLLs"
    )
endif()

# Orbbec SDK DLLs
if(ORBBEC_SDK_FOUND AND EXISTS "${ORBBEC_LIB_DIR}")
    add_custom_command(TARGET ManualGripper POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${ORBBEC_LIB_DIR}"
            "$<TARGET_FILE_DIR:ManualGripper>"
        COMMENT "Copying Orbbec SDK DLLs"
    )
endif()

add_custom_command(TARGET ManualGripper POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${MSYS2_ROOT}/bin/libgcc_s_seh-1.dll" "$<TARGET_FILE_DIR:ManualGripper>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${MSYS2_ROOT}/bin/libstdc++-6.dll" "$<TARGET_FILE_DIR:ManualGripper>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${MSYS2_ROOT}/bin/libwinpthread-1.dll" "$<TARGET_FILE_DIR:ManualGripper>"
    COMMENT "Copying MinGW runtime DLLs"
)

# 复制 OpenCV 直接依赖 DLL
foreach(_cv_lib ${OpenCV_LIBS})
    get_target_property(_cv_lib_loc ${_cv_lib} LOCATION)
    if(_cv_lib_loc)
        add_custom_command(TARGET ManualGripper POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_cv_lib_loc}" "$<TARGET_FILE_DIR:ManualGripper>"
        )
    endif()
endforeach()

# 广成科技 CAN SDK DLL
if(EXISTS "${CMAKE_SOURCE_DIR}/lib/gcan/ECanVci64.dll")
    add_custom_command(TARGET ManualGripper POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/lib/gcan/ECanVci64.dll" "$<TARGET_FILE_DIR:ManualGripper>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/lib/gcan/CHUSBDLL64.dll" "$<TARGET_FILE_DIR:ManualGripper>"
        COMMENT "Copying GCAN CAN SDK DLL"
    )
endif()

# 复制前端文件和配置文件
add_custom_command(TARGET ManualGripper POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/frontend" "$<TARGET_FILE_DIR:ManualGripper>/../frontend"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/config.json" "$<TARGET_FILE_DIR:ManualGripper>/../config.json"
    COMMENT "Copying frontend and config files"
)

message(STATUS "OpenCV include: ${OpenCV_INCLUDE_DIRS}")
message(STATUS "MSYS2 root: ${MSYS2_ROOT}")
message(STATUS "Tip: Run collect_dlls.ps1 after build to collect all transitive DLL dependencies")
