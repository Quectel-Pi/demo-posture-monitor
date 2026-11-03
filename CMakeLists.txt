cmake_minimum_required(VERSION 3.16)
project(posture_guard LANGUAGES CXX)

# ---------------------------------------------------------------------------
# RK3576 坐姿检测与护眼提醒系统
#
# 设计依据：docs/architecture.md 与《RK3576 坐姿检测与护眼提醒_完整实施方案》
#   §6.1 目录、§6.3 数据契约、§11 数据结构与状态机、§13.2 systemd 部署
#
# 依赖：OpenCV（预处理/摄像头）、librknnrt.so（NPU 推理）、pthread。
#      不引入 yaml-cpp / nlohmann：板端没有这些包，配置解析用内置的极简
#      子集解析器（见 src/Yaml.cpp），JSON 输出用手写序列化（见 src/Json.cpp）。
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

find_package(OpenCV REQUIRED COMPONENTS core imgproc videoio imgcodecs)
find_package(Threads REQUIRED)

# --- RKNN Runtime -----------------------------------------------------------
# 优先使用 vendor/rknn 下的头文件（与板端 librknnrt.so 版本严格配对），
# 库从系统路径找，找不到就用 vendor/rknn/lib 下的副本。
set(RKNN_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/rknn" CACHE PATH "rknn_api.h dir")
find_library(RKNN_LIBRARY NAMES rknnrt
  HINTS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/rknn/lib" /usr/lib /usr/local/lib)

if(NOT EXISTS "${RKNN_INCLUDE_DIR}/rknn_api.h")
  message(FATAL_ERROR "缺少 ${RKNN_INCLUDE_DIR}/rknn_api.h，先执行 scripts/fetch_deps.sh")
endif()
if(NOT RKNN_LIBRARY)
  message(FATAL_ERROR "找不到 librknnrt.so，先执行 scripts/fetch_deps.sh 或安装板端 SDK")
endif()
message(STATUS "RKNN include : ${RKNN_INCLUDE_DIR}")
message(STATUS "RKNN library : ${RKNN_LIBRARY}")
message(STATUS "OpenCV       : ${OpenCV_VERSION}")

# --- 核心库（daemon 与测试共用，便于单独测试规则） --------------------------
add_library(posture_core STATIC
  src/Types.cpp
  src/Logger.cpp
  src/Yaml.cpp
  src/Json.cpp
  src/Config.cpp
  src/V4l2Probe.cpp
  src/CameraCapture.cpp
  src/RknnPoseEngine.cpp
  src/PostureProfile.cpp
  src/PostureAnalyzer.cpp
  src/UserCalibration.cpp
  src/EyeCareMonitor.cpp
  src/ReminderManager.cpp
)
target_include_directories(posture_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${RKNN_INCLUDE_DIR}
  ${OpenCV_INCLUDE_DIRS}
)
target_link_libraries(posture_core PUBLIC
  ${OpenCV_LIBS}
  ${RKNN_LIBRARY}
  Threads::Threads
)
# 模型体积大，但推理走 NPU；让运行时能在 vendor 目录里找到库
set_target_properties(posture_core PROPERTIES
  BUILD_RPATH "${CMAKE_CURRENT_SOURCE_DIR}/vendor/rknn/lib")

# --- 可执行程序 -------------------------------------------------------------
add_executable(posture_guard
  src/main.cpp
  src/Notifier.cpp
)
target_link_libraries(posture_guard PRIVATE posture_core)
target_include_directories(posture_guard PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
set_target_properties(posture_guard PROPERTIES
  BUILD_RPATH "${CMAKE_CURRENT_SOURCE_DIR}/vendor/rknn/lib"
  INSTALL_RPATH "/opt/posture-guard/lib"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# --- 单元测试（不需要摄像头/NPU） ------------------------------------------
enable_testing()

add_executable(test_posture_rules tests/test_posture_rules.cpp)
target_link_libraries(test_posture_rules PRIVATE posture_core)
add_test(NAME posture_rules COMMAND test_posture_rules)

add_executable(test_session_timer tests/test_session_timer.cpp)
target_link_libraries(test_session_timer PRIVATE posture_core)
add_test(NAME session_timer COMMAND test_session_timer)

add_executable(test_config tests/test_config.cpp)
target_link_libraries(test_config PRIVATE posture_core)
add_test(NAME config COMMAND test_config ${CMAKE_CURRENT_SOURCE_DIR}/config/posture.yaml)

# 画像级联迟滞（PostureProfile::ClassifyHysteretic）—— 纯逻辑，无摄像头/NPU 依赖
add_executable(test_profile_hysteresis tests/test_profile_hysteresis.cpp)
target_link_libraries(test_profile_hysteresis PRIVATE posture_core)
add_test(NAME profile_hysteresis COMMAND test_profile_hysteresis
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# UI 上行命令解析（ack / snooze / calibrate）
# 这类错误是静默失败：解析不出来的命令既不报错也不记录。
# 实测踩过「我已经休息好了」按钮完全无反应（ExtractString 只认无空格 JSON），
# 12 小时生产日志里「用户确认已休息」出现 0 次。必须有回归保护。
add_executable(test_command_parse tests/test_command_parse.cpp)
add_test(NAME command_parse COMMAND test_command_parse)

add_executable(test_rknn_decode tests/test_rknn_decode.cpp)
target_link_libraries(test_rknn_decode PRIVATE posture_core)
add_test(NAME rknn_decode COMMAND test_rknn_decode)

install(TARGETS posture_guard RUNTIME DESTINATION bin)
