# CMakeLists.txt - UMI 手动夹爪驱动库构建配置
# 构建目标：umi 动态共享库（包含 V1 和 V2 驱动）
# 依赖：glog（日志）、serial（串口通信）、common::tool（工具库）
# 可选测试：基于 GoogleTest 的 V1/V2 驱动测试

# 从源码目录路径提取当前文件夹名作为目标名
string(REGEX REPLACE ".*/\(.*\)" "\\1" CURRENT_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# 根据目录路径生成命名空间限定的目标名和别名
get_namespace(${CMAKE_CURRENT_SOURCE_DIR} CURRENT_TARGET_NAME)
get_alias_namespace(${CMAKE_CURRENT_SOURCE_DIR} CURRENT_TARGET_ALIAS_NAME)

# 递归收集头文件（.h, .hpp）
file(GLOB_RECURSE head_files ${CMAKE_CURRENT_SOURCE_DIR}/*.h ${CMAKE_CURRENT_SOURCE_DIR}/*.hpp)
# 递归收集源文件（.cc, .cpp）
file(GLOB_RECURSE souce_files ${CMAKE_CURRENT_SOURCE_DIR}/*.cc ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
# 递归收集测试文件（test_*.cc, test_*.cpp）
file(GLOB_RECURSE test_files  ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cc ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp)
# 从源文件列表中排除测试文件（测试单独构建为可执行文件）
list(REMOVE_ITEM souce_files ${test_files})

# 构建动态共享库
add_library(${CURRENT_TARGET_NAME} SHARED)
add_library(${CURRENT_TARGET_ALIAS_NAME} ALIAS ${CURRENT_TARGET_NAME})

# 设置库的源文件
target_sources(${CURRENT_TARGET_NAME} PRIVATE ${souce_files})

# 设置库的头文件（供其他目标引用）
target_sources(${CURRENT_TARGET_NAME} INTERFACE ${head_files})

# 设置头文件搜索路径：当前目录的上一级（使得可以用 #include "umi/umi_driver.h" 引用）
target_include_directories(${CURRENT_TARGET_NAME}
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
)

# 链接依赖库：glog（日志）、serial（串口）、common::tool（CRC/时间等工具）
target_link_libraries(${CURRENT_TARGET_NAME}
  PUBLIC
    glog::glog
    src::hardware::third_party::serial
    src::common::tool
)

# 安装规则：将库安装到 bin（运行时）和 lib（库文件）目录
install(TARGETS ${CURRENT_TARGET_NAME} OPTIONAL
  EXPORT ${CURRENT_TARGET_ALIAS_NAME}
  RUNTIME DESTINATION bin
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
)

# 测试构建（仅在 BUILD_TESTS=ON 时启用）
# 为每个 test_*.cc 文件生成独立的 GoogleTest 可执行文件
if(BUILD_TESTS AND test_files)
foreach(test_file ${test_files})
  get_filename_component(FILE_BASE ${test_file} NAME_WE)
  add_googletest(${FILE_BASE}
    SRCS
      ${test_file}
    DEPS
      GTest::GTest
      GTest::Main
      glog::glog
      src::common::tool
      src::hardware::hikvision
      ${CURRENT_TARGET_NAME}
  )
endforeach()
endif()
