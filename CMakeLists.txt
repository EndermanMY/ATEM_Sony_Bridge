cmake_minimum_required(VERSION 3.15)

project(AtemSonyBridge
    VERSION 0.2.1
    DESCRIPTION "ATEM to Sony Camera Bridge"
    LANGUAGES CXX
)

set(BRIDGE_PRERELEASE "dev" CACHE STRING "Prerelease suffix, empty for a final release")
set(BRIDGE_VERSION "${PROJECT_VERSION}")
if(BRIDGE_PRERELEASE)
    string(APPEND BRIDGE_VERSION "-${BRIDGE_PRERELEASE}")
endif()
file(WRITE "${CMAKE_BINARY_DIR}/bridge-version.txt" "${BRIDGE_VERSION}")
configure_file(src/common/Version.h.in generated/Version.h @ONLY)
configure_file(src/common/Version.rc.in generated/Version.rc @ONLY)
include_directories(${CMAKE_BINARY_DIR}/generated)

# C++17 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 编译选项
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/W4 /WX- /permissive- /utf-8)
    # 避免 windows.h 的 min/max 宏与标准库冲突；
    # UNICODE 必须定义：CrTypes.h 以此选择 CrChar = wchar_t（宽字符）分支
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN WIN32 UNICODE _UNICODE)
    # 纯窗口程序（无控制台），入口仍为 main()
    # Window subsystem is applied only to the desktop executable below.
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# 输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Sony CrSDK 配置
set(SONY_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/sony_sdk)
set(SONY_SDK_INCLUDE_DIR ${SONY_SDK_ROOT}/include)
set(SONY_SDK_LIB_DIR ${SONY_SDK_ROOT}/lib/x64)
set(SONY_SDK_BIN_DIR ${SONY_SDK_ROOT}/bin/x64)

if(NOT EXISTS ${SONY_SDK_INCLUDE_DIR}/CameraRemote_SDK.h)
    message(FATAL_ERROR "Sony SDK not found at ${SONY_SDK_INCLUDE_DIR}. Restore the authorized SDK locally; SDK binaries are intentionally excluded from GitHub.")
endif()

# Sony SDK 导入库
add_library(sony_sdk_core SHARED IMPORTED)
set_target_properties(sony_sdk_core PROPERTIES
    IMPORTED_LOCATION ${SONY_SDK_BIN_DIR}/Cr_Core.dll
    IMPORTED_IMPLIB ${SONY_SDK_LIB_DIR}/Cr_Core.lib
)

# 头文件目录
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${SONY_SDK_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/webview2/include
)

# 源文件
set(SOURCES
    src/main.cpp
    src/common/Logger.h
    src/config/ConfigManager.cpp
    src/config/ConfigManager.h
    src/sony/CameraController.cpp
    src/sony/CameraController.h
    src/atem/AtemController.cpp
    src/atem/AtemController.h
    src/mapping/MappingEngine.cpp
    src/mapping/MappingEngine.h
    src/service/BridgeService.cpp
    src/service/BridgeService.h
    src/ui/WebHost.cpp
    src/ui/WebHost.h
    src/discovery/NotifyDiscovery.cpp
    src/discovery/NotifyDiscovery.h
)

add_executable(${PROJECT_NAME} ${SOURCES} ${CMAKE_BINARY_DIR}/generated/Version.rc)
if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
endif()

target_link_libraries(${PROJECT_NAME}
    sony_sdk_core
    ole32 user32 gdi32 shell32   # Win32 UI
    ws2_32                 # Winsock（ATEM/BMD 虚拟相机 UDP 服务）
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/webview2/x64/WebView2LoaderStatic.lib
)

# 复制 Sony SDK 运行时 DLL 与配置文件到输出目录
# 注意布局必须与官方示例一致：Cr_Core/monitor_protocol 平铺，
# 协议适配器（Cr_PTP_*、libssh2、libusb）必须位于 CrAdapter/ 子目录，
# 否则 Cr_Core 找不到适配器插件，EnumCameraObjects 报 0x8703 (Adaptor_Create)
if(WIN32)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SONY_SDK_BIN_DIR}/Cr_Core.dll"
            "${SONY_SDK_BIN_DIR}/monitor_protocol.dll"
            "${SONY_SDK_BIN_DIR}/monitor_protocol_pf.dll"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/ui"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/ui"
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/CrAdapter"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SONY_SDK_BIN_DIR}/Cr_PTP_IP.dll"
            "${SONY_SDK_BIN_DIR}/Cr_PTP_USB.dll"
            "${SONY_SDK_BIN_DIR}/libssh2.dll"
            "${SONY_SDK_BIN_DIR}/libusb-1.0.dll"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/CrAdapter/"
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/config"
        # 只写默认模板 config.default.json，绝不覆盖用户运行时的 config.json
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/config/config.default.json"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/config/config.default.json"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/config/mapping_rules.json"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/config/mapping_rules.default.json"
        COMMENT "Copying Sony SDK DLLs (official CrAdapter layout) and config templates"
    )
endif()

message(STATUS "=== ATEM Sony Bridge Configuration ===")
message(STATUS "Sony SDK: ${SONY_SDK_INCLUDE_DIR}")
message(STATUS "================================")

# GUID 稳定性测试工具（调试用）
option(BRIDGE_BUILD_DIAGNOSTICS "Build hardware diagnostics" OFF)
if(BRIDGE_BUILD_DIAGNOSTICS)
add_executable(guidtest tools/guidtest.cpp)
target_include_directories(guidtest PRIVATE ${SONY_SDK_INCLUDE_DIR})
target_link_libraries(guidtest PRIVATE sony_sdk_core)
set_target_properties(guidtest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

endif()

include(CTest)
if(BUILD_TESTING)
add_executable(core_tests tests/core_tests.cpp src/config/ConfigManager.cpp)
add_test(NAME core_regression COMMAND core_tests)
endif()
