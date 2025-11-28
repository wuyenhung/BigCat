cmake_minimum_required(VERSION 3.25)
project(BigCat)

include(CTest)

option(BIGCAT_ENABLE_UNITY_BUILD "Enable unity build for eligible BigCat targets when supported by CMake" OFF)
option(BIGCAT_ENABLE_RELEASE_AVX2 "Build Release targets with AVX2 instructions. Leave OFF for simulation/test compatibility on older CPUs." OFF)
set(BIGCAT_TEST_TIMEOUT_SECONDS "300" CACHE STRING "Default timeout for CTest test cases, in seconds")

if(BIGCAT_ENABLE_UNITY_BUILD AND CMAKE_VERSION VERSION_LESS "3.16")
    message(WARNING "BIGCAT_ENABLE_UNITY_BUILD requires CMake 3.16+; current generator will ignore unity build requests.")
endif()

set(BIGCAT_RELEASE_AVX2_OPTION "")
if(BIGCAT_ENABLE_RELEASE_AVX2)
    set(BIGCAT_RELEASE_AVX2_OPTION $<$<CONFIG:Release>:/arch:AVX2>)
endif()

# Option: generate PDB (debug symbols) for Release-like builds
option(GENERATE_PDB_FOR_RELEASE "Generate PDB/symbols for Release and RelWithDebInfo builds (disables /GL/LTCG for Release to improve debuggability)" ON)

# Enforce MSVC toolchain: this project targets Windows and Win32 APIs and
# is tested with MSVC. Fail configure early when using other compilers.
if (NOT MSVC)
    message(WARNING "MSVC not detected. CMake will continue, but MSVC-specific optimizations and settings will be skipped. For best results, run CMake from a Visual Studio Developer Command Prompt or use the MSVC generator.")
endif()

# Set output directories per-configuration (works correctly with multi-config generators
# like Visual Studio). This ensures outputs go to build/Release (not build/Release/Release).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 設定 SOEM 路徑 (請根據實際安裝位置調整)
set(SOEM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/SOEM" CACHE PATH "Path to SOEM library")

# 設定 Npcap SDK 路徑 (可由 -DNPCAP_SDK_DIR=<path> 或環境變數 NPCAP_SDK_DIR 覆寫)
set(NPCAP_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/npcap-sdk-1.15" CACHE PATH "Path to Npcap SDK")

# --- 新增：更健壯的 Npcap 偵測與 find_* 邏輯 -----------------------------
# Allow override from environment if the cache value is empty or default doesn't exist
if(NOT EXISTS "${NPCAP_SDK_DIR}/Include/pcap.h")
    if(DEFINED ENV{NPCAP_SDK_DIR} AND NOT "" STREQUAL "$ENV{NPCAP_SDK_DIR}")
        set(NPCAP_SDK_DIR $ENV{NPCAP_SDK_DIR})
    endif()
endif()

if(NOT EXISTS "${NPCAP_SDK_DIR}/Include/pcap.h")
    set(_possible_npcap_paths
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/npcap-sdk-1.15"
        "C:/npcap-sdk"
        "C:/WpdPack"
        "$ENV{ProgramFiles}/Npcap"
        "$ENV{ProgramFiles(x86)}/Npcap"
    )
    foreach(_p ${_possible_npcap_paths})
        if(EXISTS "${_p}/Include/pcap.h")
            set(NPCAP_SDK_DIR "${_p}")
            break()
        endif()
    endforeach()
endif()

if(EXISTS "${NPCAP_SDK_DIR}/Include/pcap.h")
    message(STATUS "Using Npcap SDK at: ${NPCAP_SDK_DIR}")
    find_path(NPCAP_INCLUDE_DIR pcap.h HINTS "${NPCAP_SDK_DIR}/Include" PATH_SUFFIXES "")
    # Try both common lib locations (root Lib and Lib/x64)
    find_library(WPCAP_LIB NAMES wpcap HINTS "${NPCAP_SDK_DIR}/Lib" "${NPCAP_SDK_DIR}/Lib/x64" PATH_SUFFIXES "")
    find_library(PACKET_LIB NAMES Packet Packet32 HINTS "${NPCAP_SDK_DIR}/Lib" "${NPCAP_SDK_DIR}/Lib/x64" PATH_SUFFIXES "")
else()
    message(WARNING "Npcap SDK not found. You can set -DNPCAP_SDK_DIR=<path> or install Npcap SDK (https://npcap.com/). Falling back to original behavior which assumes linker can find wpcap/Packet.")
endif()

# -------------------------------------------------------------------------

# 包含目錄
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${SOEM_DIR}/include
    ${SOEM_DIR}/build/include
    ${SOEM_DIR}/osal
    ${SOEM_DIR}/osal/win32
    ${SOEM_DIR}/oshw/win32
    ${NPCAP_SDK_DIR}/Include
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/tinyxml2
)


# Add Windows SDK and MSVC include paths explicitly for MSVC
if(MSVC)
    # When building outside of Developer Command Prompt, CMake may not find Windows SDK headers
    # First, add MSVC compiler include paths
    # Compiler path example: C:/Program Files/Microsoft Visual Studio/18/Insiders/VC/Tools/MSVC/14.50.35503/bin/Hostx64/x64/cl.exe
    # We need: C:/Program Files/Microsoft Visual Studio/18/Insiders/VC/Tools/MSVC/14.50.35503/include
    if(CMAKE_CXX_COMPILER)
        get_filename_component(COMPILER_DIR ${CMAKE_CXX_COMPILER} DIRECTORY)
        # Go up: bin/Hostx64/x64 -> bin/Hostx64 -> bin -> MSVC version root
        get_filename_component(MSVC_HOST_DIR ${COMPILER_DIR} DIRECTORY)
        get_filename_component(MSVC_BIN_DIR ${MSVC_HOST_DIR} DIRECTORY)
        get_filename_component(MSVC_VERSION_DIR ${MSVC_BIN_DIR} DIRECTORY)
        set(MSVC_INCLUDE_DIR "${MSVC_VERSION_DIR}/include")
        if(EXISTS "${MSVC_INCLUDE_DIR}")
            include_directories("${MSVC_INCLUDE_DIR}")
            message(STATUS "Added MSVC include path: ${MSVC_INCLUDE_DIR}")
        endif()
    endif()
    
    # Then, try to find the Windows SDK automatically.
    # Windows Kits/10/Include also contains non-version folders such as 'wdf';
    # only numeric SDK version directories are valid here.
    set(_bigcat_winsdk_version "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
    if(_bigcat_winsdk_version AND NOT _bigcat_winsdk_version MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+$")
        message(WARNING "Ignoring non-version Windows SDK identifier: ${_bigcat_winsdk_version}")
        unset(_bigcat_winsdk_version)
        unset(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
    endif()

    if(NOT _bigcat_winsdk_version)
        file(GLOB SDK_VERSION_PATHS LIST_DIRECTORIES true "C:/Program Files (x86)/Windows Kits/10/Include/*")
        set(SDK_VERSIONS)
        foreach(SDK_PATH ${SDK_VERSION_PATHS})
            if(IS_DIRECTORY "${SDK_PATH}")
                get_filename_component(SDK_VERSION "${SDK_PATH}" NAME)
                if(SDK_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+$")
                    list(APPEND SDK_VERSIONS "${SDK_VERSION}")
                endif()
            endif()
        endforeach()

        if(SDK_VERSIONS)
            list(SORT SDK_VERSIONS)
            list(REVERSE SDK_VERSIONS)
            list(GET SDK_VERSIONS 0 _bigcat_winsdk_version)
            set(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION "${_bigcat_winsdk_version}")
            message(STATUS "Detected Windows SDK version: ${_bigcat_winsdk_version}")
        endif()
    endif()
    
    # Add Windows SDK paths if found
    if(DEFINED CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
        set(WINSDK_ROOT "C:/Program Files (x86)/Windows Kits/10")
        if(EXISTS "${WINSDK_ROOT}/Include/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
            include_directories(
                "${WINSDK_ROOT}/Include/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/ucrt"
                "${WINSDK_ROOT}/Include/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/um"
                "${WINSDK_ROOT}/Include/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/shared"
            )
            # Add library paths for Windows SDK
            link_directories(
                "${WINSDK_ROOT}/Lib/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/ucrt/x64"
                "${WINSDK_ROOT}/Lib/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/um/x64"
            )
            message(STATUS "Added Windows SDK include paths: ${WINSDK_ROOT}/Include/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
            message(STATUS "Added Windows SDK library paths: ${WINSDK_ROOT}/Lib/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
        endif()
    else()
        message(WARNING "Windows SDK not found. Please run CMake from Visual Studio Developer Command Prompt or use build.ps1")
    endif()
    
    # Add MSVC library paths
    if(MSVC_VERSION_DIR AND EXISTS "${MSVC_VERSION_DIR}/lib/x64")
        link_directories("${MSVC_VERSION_DIR}/lib/x64")
        message(STATUS "Added MSVC library path: ${MSVC_VERSION_DIR}/lib/x64")
    endif()
endif()

# Apply MSVC whole-program optimization flags globally for Release builds so
# that subprojects (SOEM, tinyxml2, etc.) inherit /GL and the final link uses /LTCG.
if(MSVC)
    if(GENERATE_PDB_FOR_RELEASE)
        message(STATUS "GENERATE_PDB_FOR_RELEASE=ON: Building Release/RelWithDebInfo with debug symbols (PDB). This will disable /GL /LTCG for Release to improve debuggability.")
        # Use optimization but preserve debug info; avoid /GL and /LTCG which hinder source-level debug info
        add_compile_options(
            $<$<CONFIG:Release>:/O2>
            $<$<CONFIG:Release>:/Zi>
            $<$<CONFIG:RelWithDebInfo>:/O2>
            $<$<CONFIG:RelWithDebInfo>:/Zi>
            ${BIGCAT_RELEASE_AVX2_OPTION}
            $<$<CONFIG:Release>:/fp:fast>
        )

        # Ask the linker to generate PDB files for Release/RelWithDebInfo
        set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /DEBUG /INCREMENTAL:NO")
        set(CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO "${CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO} /DEBUG /INCREMENTAL:NO")
        set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /DEBUG /INCREMENTAL:NO")
    else()
        # Global compile options applied directory-wide (subprojects inherit these)
        add_compile_options(
            $<$<CONFIG:Release>:/O2>
            $<$<CONFIG:Release>:/GL>
            ${BIGCAT_RELEASE_AVX2_OPTION}
            $<$<CONFIG:Release>:/fp:fast>
        )

        # Ensure the linker is invoked with LTCG for Release executables and libraries
        # and disable incremental linking which conflicts with LTCG benefits.
        set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /LTCG /INCREMENTAL:NO")
        set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /LTCG /INCREMENTAL:NO")
        set(CMAKE_STATIC_LINKER_FLAGS_RELEASE "${CMAKE_STATIC_LINKER_FLAGS_RELEASE} /LTCG")
    endif()
endif()

# NOTE: Build the SOEM library as a separate target instead of globbing and
# compiling its C sources directly into this executable. Compiling the same
# sources in multiple targets causes MSVC to place object files in a shared
# intermediate directory which can trigger MSB8028 (shared intermediate
# directory) warnings. Use add_subdirectory to build the SOEM target and
# link against it.
# Set unique output directories for SOEM to avoid conflicts
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/bin/soem)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/lib/soem)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/lib/soem)
if(MSVC)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_NO_WARNINGS TRUE)
endif()
add_subdirectory(${SOEM_DIR})
# Reset for main project
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/bin/main)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/lib/main)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/lib/main)

# 專案源文件：先抽出可重用模組 target，再由主程式組裝 UI / server / app。
set(BIGCAT_CORE_SOURCES
    src/core/AppConfig.cpp
    src/core/CoreHelpers.cpp
    src/core/CpuIsolation.cpp
    src/core/Win32Utils.cpp
)

set(BIGCAT_IO_SOURCES
    src/io/MMFReader.cpp
    src/io/MMFWriter.cpp
    src/io/StatusLogger.cpp
    src/io/MachineConfigIO.cpp
)

set(BIGCAT_SECURITY_SOURCES
    src/security/LicenseChecker.cpp
)

set(BIGCAT_ETHERCAT_SOURCES
    src/ethercat/EtherCATMaster.cpp
    src/ethercat/EtherCATMaster.Init.cpp
    src/ethercat/EtherCATMaster.Cyclic.cpp
    src/ethercat/EtherCATMaster.SDO.cpp
    src/ethercat/EtherCATMaster.Simulation.cpp
    src/ethercat/EtherCATMaster.MMF.cpp
    src/ethercat/EtherCATMaster.Diagnostics.cpp
    src/ethercat/PDOFilter.cpp
    src/ethercat/PDOHelpers.cpp
    src/ethercat/EsiParser.cpp
)

set(BIGCAT_MOTION_SOURCES
    src/motion/MotionController.cpp
    src/motion/MotionConfigIO.cpp
    src/motion/MotionLogic.cpp
    src/motion/StewartIK.cpp
)

set(BIGCAT_SERVER_SUPPORT_SOURCES
    src/server/HttpServer.API.Helpers.cpp
    src/server/HttpServer.Static.Helpers.cpp
    src/server/HttpServer.Static.cpp
    src/server/HttpServer.Auth.cpp
)

set(BIGCAT_APP_SOURCES
    src/app/AppController.cpp
)

set(BIGCAT_SERVER_SOURCES
    src/server/HttpServer.Core.cpp
    src/server/HttpServer.API.cpp
    src/server/HttpServer.SSE.cpp
)

set(BIGCAT_UI_SOURCES
    src/ui/MainWindow.cpp
    src/ui/MainWindow.Command.cpp
    src/server/MainWindow.HTTP.cpp
    src/ui/MainWindow.Subclass.cpp
    src/ui/AutoWindow.cpp
    src/ui/MainWindow.Logic.cpp
    src/ui/MainWindow.Connect.cpp
    src/ui/MainWindow.AutoStart.cpp
    src/ui/MainWindow.Motion.cpp
    src/ui/MainWindow.SDO.cpp
    src/ui/MainWindow.Timer.cpp
    src/ui/MainWindow.Watchdog.cpp
    src/ui/MainWindow.ESI.cpp
    src/ui/MainWindow.Adapter.cpp
    src/ui/MainWindow.Master.cpp
    src/ui/PDOProxy.cpp
    src/ui/Win32Measure.cpp
    src/ui/StatusPane.cpp
    src/ui/Win32WindowCore.cpp
)

set(BIGCAT_MAIN_SOURCES
    src/pch.cpp
    src/app/main.cpp
    resources/airwolf.rc
)

# Option to add an automatic crash handler which writes a minidump on unhandled exceptions
option(ENABLE_CRASH_HANDLER "Enable automatic minidump crash handler on Windows (MSVC)" ON)

if(ENABLE_CRASH_HANDLER AND MSVC AND WIN32)
    list(APPEND BIGCAT_MAIN_SOURCES src/app/CrashHandler.cpp)
    message(STATUS "ENABLE_CRASH_HANDLER=ON: will compile crash handler and link dbghelp")
else()
    message(STATUS "ENABLE_CRASH_HANDLER is OFF or not applicable (non-MSVC/WIN32)")
endif()

# 專案標頭檔
set(PROJECT_HEADERS
    include/EtherCATMaster.h
    include/MainWindow.h
    include/MotionController.h
    include/MotionConfigIO.h
    include/EsiParser.h
    include/Win32Utils.h
    include/PDOFilter.h
    include/core/AppConfig.h
    include/core/CpuIsolation.h
    # include/AutoProcedure.h (removed)
)

# tinyxml2: default to fetching via FetchContent to ensure full implementation.
# If you want to use a local copy under lib/tinyxml2, set -DUSE_LOCAL_TINYXML2=ON
option(USE_LOCAL_TINYXML2 "Use local lib/tinyxml2 in the repository" OFF)
set(TINYXML2_LOCAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/tinyxml2")
if(USE_LOCAL_TINYXML2)
    if(EXISTS "${TINYXML2_LOCAL_DIR}/tinyxml2.cpp")
        message(STATUS "Using local tinyxml2 from ${TINYXML2_LOCAL_DIR}")
        add_library(tinyxml2 STATIC "${TINYXML2_LOCAL_DIR}/tinyxml2.cpp")
        target_include_directories(tinyxml2 PUBLIC "${TINYXML2_LOCAL_DIR}")
    else()
        message(FATAL_ERROR "USE_LOCAL_TINYXML2=ON but ${TINYXML2_LOCAL_DIR}/tinyxml2.cpp not found")
    endif()
else()
    include(FetchContent)
    FetchContent_Declare(
        tinyxml2
        GIT_REPOSITORY https://github.com/leethomason/tinyxml2.git
        GIT_TAG        9.0.0
    )
    FetchContent_MakeAvailable(tinyxml2)
    # FetchContent provides target tinyxml2::tinyxml2
    if (NOT TARGET tinyxml2::tinyxml2)
        message(FATAL_ERROR "FetchContent did not provide tinyxml2 target")
    endif()
endif()

add_library(bigcat_common INTERFACE)
target_include_directories(bigcat_common INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${SOEM_DIR}/include
    ${SOEM_DIR}/build/include
    ${SOEM_DIR}/osal
    ${SOEM_DIR}/osal/win32
    ${SOEM_DIR}/oshw/win32
    ${NPCAP_SDK_DIR}/Include
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/tinyxml2
)

if(DEFINED NPCAP_INCLUDE_DIR)
    target_include_directories(bigcat_common INTERFACE ${NPCAP_INCLUDE_DIR})
endif()

target_compile_features(bigcat_common INTERFACE cxx_std_17)

if(MSVC)
    target_compile_options(bigcat_common INTERFACE /W4 /wd4996 /wd4819 /wd4201 /wd4244 /utf-8)
    target_compile_definitions(bigcat_common INTERFACE
        _CRT_SECURE_NO_WARNINGS
        UNICODE
        _UNICODE
        WIN32
        HAVE_REMOTE
        WPCAP
        WIN32_LEAN_AND_MEAN
    )
else()
    target_compile_options(bigcat_common INTERFACE -Wall -Wextra)
endif()

function(bigcat_configure_target target_name)
    target_link_libraries(${target_name} PUBLIC bigcat_common)

    if(BIGCAT_ENABLE_UNITY_BUILD AND CMAKE_VERSION VERSION_GREATER_EQUAL "3.16")
        set_property(TARGET ${target_name} PROPERTY UNITY_BUILD ON)
    endif()
endfunction()

function(bigcat_configure_test_target target_name)
    bigcat_configure_target(${target_name})
endfunction()

function(bigcat_add_optional_test target_name source_file test_labels)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${source_file}")
        message(STATUS "Skipping optional test target ${target_name}: missing ${source_file}")
        return()
    endif()

    add_executable(${target_name}
        ${source_file}
    )
    bigcat_configure_test_target(${target_name})

    if(ARGN)
        target_link_libraries(${target_name} PRIVATE ${ARGN})
    endif()

    add_test(NAME ${target_name} COMMAND $<TARGET_FILE:${target_name}>)
    set_tests_properties(${target_name} PROPERTIES
        LABELS "${test_labels}"
        TIMEOUT "${BIGCAT_TEST_TIMEOUT_SECONDS}"
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:${target_name}>"
    )
endfunction()

add_library(bigcat_core STATIC ${BIGCAT_CORE_SOURCES})
bigcat_configure_target(bigcat_core)

add_library(bigcat_io STATIC ${BIGCAT_IO_SOURCES})
bigcat_configure_target(bigcat_io)
target_link_libraries(bigcat_io PUBLIC bigcat_core)

add_library(bigcat_security STATIC ${BIGCAT_SECURITY_SOURCES})
bigcat_configure_target(bigcat_security)

add_library(bigcat_ethercat STATIC ${BIGCAT_ETHERCAT_SOURCES})
bigcat_configure_target(bigcat_ethercat)
target_link_libraries(bigcat_ethercat PUBLIC
    bigcat_core
    bigcat_io
    soem
)
if(USE_LOCAL_TINYXML2)
    target_link_libraries(bigcat_ethercat PUBLIC tinyxml2)
else()
    target_link_libraries(bigcat_ethercat PUBLIC tinyxml2::tinyxml2)
endif()

add_library(bigcat_motion STATIC ${BIGCAT_MOTION_SOURCES})
bigcat_configure_target(bigcat_motion)
target_link_libraries(bigcat_motion PUBLIC
    bigcat_core
    bigcat_ethercat
)

add_library(bigcat_server_support STATIC ${BIGCAT_SERVER_SUPPORT_SOURCES})
bigcat_configure_target(bigcat_server_support)
target_link_libraries(bigcat_server_support PUBLIC bigcat_core)

add_library(bigcat_app STATIC ${BIGCAT_APP_SOURCES})
bigcat_configure_target(bigcat_app)
target_link_libraries(bigcat_app PUBLIC
    bigcat_core
    bigcat_motion
    bigcat_ethercat
)

add_library(bigcat_server STATIC ${BIGCAT_SERVER_SOURCES})
bigcat_configure_target(bigcat_server)
target_link_libraries(bigcat_server PUBLIC
    bigcat_core
    bigcat_app
    bigcat_ethercat
    bigcat_server_support
)

add_library(bigcat_ui STATIC ${BIGCAT_UI_SOURCES})
bigcat_configure_target(bigcat_ui)
target_link_libraries(bigcat_ui PUBLIC
    bigcat_core
    bigcat_io
    bigcat_security
    bigcat_motion
    bigcat_ethercat
    bigcat_app
    bigcat_server
)

# tools: generate_license — create license.txt from fingerprint
# Build as a WIN32 GUI app (no console) and link common GUI libs
add_executable(generate_license WIN32 tools/generate_license.cpp)
bigcat_configure_target(generate_license)
set_target_properties(generate_license PROPERTIES OUTPUT_NAME "generate_license" WIN32_EXECUTABLE TRUE)
target_link_libraries(generate_license PRIVATE bigcat_security bcrypt comdlg32 user32)

# 建立可執行檔
add_executable(${PROJECT_NAME}
    ${BIGCAT_MAIN_SOURCES}
    ${PROJECT_HEADERS}
)
bigcat_configure_target(${PROJECT_NAME})
target_link_libraries(${PROJECT_NAME} PRIVATE
    bigcat_ui
    bigcat_server
    bigcat_app
    bigcat_motion
    bigcat_ethercat
    bigcat_server_support
    bigcat_io
    bigcat_security
    bigcat_core
    ws2_32
    winmm
    tbs
    ncrypt
    bcrypt
    comctl32
    setupapi
    iphlpapi
    shlwapi
    $<$<BOOL:${WPCAP_LIB}>:${WPCAP_LIB}>
    $<$<BOOL:${PACKET_LIB}>:${PACKET_LIB}>
    $<$<NOT:$<BOOL:${WPCAP_LIB}>>:wpcap>
    $<$<NOT:$<BOOL:${PACKET_LIB}>>:Packet>
    avrt
)

if(MSVC)
    target_precompile_headers(${PROJECT_NAME} PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/include/pch.h>"
    )
endif()

# Produce executable named BigCat.exe (keeps target name but sets output name)
set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "BigCat")

# If crash handler requested and we're on MSVC/Windows, link dbghelp for minidump support
if(ENABLE_CRASH_HANDLER AND MSVC AND WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE dbghelp)
endif()

# 設定 Win32 應用程式
if(WIN32)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        WIN32_EXECUTABLE TRUE
    )
    # SimpleTest 是控制台應用程式，不需要 WIN32_EXECUTABLE
endif()

# 設定編譯選項
if(MSVC)
    # --- 高效能優化（僅對 Release 有效） -----------------------------------------
    # 使用 MSVC 的全優化與連結時最佳化（LTCG）、啟用 AVX2 指令集、快速浮點模式
    # 這些選項會提高整體效能，但可能增加編譯/連結時間並使除錯困難。
    if(GENERATE_PDB_FOR_RELEASE)
        target_compile_options(${PROJECT_NAME} PRIVATE
            $<$<CONFIG:Release>:/O2>
            $<$<CONFIG:Release>:/Zi>
            $<$<CONFIG:RelWithDebInfo>:/O2>
            $<$<CONFIG:RelWithDebInfo>:/Zi>
            ${BIGCAT_RELEASE_AVX2_OPTION}
            $<$<CONFIG:Release>:/fp:fast>
            $<$<CONFIG:Release>:/Ot>
        )
        target_link_options(${PROJECT_NAME} PRIVATE
            $<$<CONFIG:Release>:/DEBUG>
            $<$<CONFIG:RelWithDebInfo>:/DEBUG>
            $<$<CONFIG:Release>:/INCREMENTAL:NO>
        )
    else()
        target_compile_options(${PROJECT_NAME} PRIVATE
            $<$<CONFIG:Release>:/O2>
            $<$<CONFIG:Release>:/GL>
            ${BIGCAT_RELEASE_AVX2_OPTION}
            $<$<CONFIG:Release>:/fp:fast>
            $<$<CONFIG:Release>:/Ot>
        )
        target_link_options(${PROJECT_NAME} PRIVATE
            $<$<CONFIG:Release>:/LTCG>
            $<$<CONFIG:Release>:/INCREMENTAL:NO>
        )
    endif()

    # MSVC Debug configuration: enable full debug info and runtime checks
    # to make crashes easier to diagnose when running under debugger or
    # producing minidumps.
    target_compile_options(${PROJECT_NAME} PRIVATE
        $<$<CONFIG:Debug>:/Zi>
        $<$<CONFIG:Debug>:/Od>
        $<$<CONFIG:Debug>:/EHa>
        $<$<CONFIG:Debug>:/RTC1>
    )
    target_link_options(${PROJECT_NAME} PRIVATE
        $<$<CONFIG:Debug>:/DEBUG>
    )
endif()

if(BUILD_TESTING)
    bigcat_add_optional_test(test_httpserver_auth tests/test_httpserver_auth.cpp "server;auth" bigcat_server_support)
    bigcat_add_optional_test(test_httpserver_static tests/test_httpserver_static.cpp "server;static" bigcat_server_support ws2_32)
    bigcat_add_optional_test(test_httpserver_api_helpers tests/test_httpserver_api_helpers.cpp "server;api" bigcat_server_support)
    bigcat_add_optional_test(test_httpserver_api_actions tests/test_httpserver_api_actions.cpp "server;api" bigcat_server_support)
    bigcat_add_optional_test(test_appcontroller_contract tests/test_appcontroller_contract.cpp "smoke;appcontroller")
    bigcat_add_optional_test(test_system_state_policy tests/test_system_state_policy.cpp "smoke;policy")
    bigcat_add_optional_test(test_coordinate_transform tests/test_coordinate_transform.cpp "smoke;math" bigcat_motion)
    bigcat_add_optional_test(test_machine_config_mmf_pose_coordinate tests/test_machine_config_mmf_pose_coordinate.cpp "smoke;motionconfig" bigcat_motion iphlpapi)
    bigcat_add_optional_test(test_machine_config_xml_io tests/test_machine_config_xml_io.cpp "smoke;motionconfig" bigcat_motion iphlpapi)
    bigcat_add_optional_test(test_mmf_pose6d_input_order tests/test_mmf_pose6d_input_order.cpp "smoke;motionconfig" bigcat_motion)
    bigcat_add_optional_test(test_motion_config_defaults tests/test_motion_config_defaults.cpp "smoke;motionconfig" bigcat_motion iphlpapi)
    bigcat_add_optional_test(test_stewart_ik tests/test_stewart_ik.cpp "smoke;math" bigcat_motion)
    bigcat_add_optional_test(test_yaw_roll_modal_diagnostic tests/test_yaw_roll_modal_diagnostic.cpp "math;diagnostic" bigcat_motion)
    bigcat_add_optional_test(test_csv_disable_filter_clears_command tests/test_csv_disable_filter_clears_command.cpp "smoke;ethercat" bigcat_motion bigcat_ethercat)
    bigcat_add_optional_test(test_simulation_reconnect_clears_staged_outputs tests/test_simulation_reconnect_clears_staged_outputs.cpp "smoke;ethercat;simulation" bigcat_motion bigcat_ethercat)
    bigcat_add_optional_test(test_simulation_softstart_no_fatal_tracking tests/test_simulation_softstart_no_fatal_tracking.cpp "smoke;ethercat;simulation" bigcat_motion bigcat_ethercat)
    bigcat_add_optional_test(test_simulation_csv_softstart_wrap_continuity tests/test_simulation_csv_softstart_wrap_continuity.cpp "smoke;ethercat;simulation" bigcat_motion bigcat_ethercat)
    bigcat_add_optional_test(test_simulation_csv_hard_limit_no_outward_velocity tests/test_simulation_csv_hard_limit_no_outward_velocity.cpp "smoke;ethercat;simulation" bigcat_motion bigcat_ethercat)
    bigcat_add_optional_test(test_simulation_csv_softstart_hard_limit_prebrake tests/test_simulation_csv_softstart_hard_limit_prebrake.cpp "smoke;ethercat;simulation" bigcat_motion bigcat_ethercat)
    bigcat_add_optional_test(test_simulation_inverted_axis_softstart_boundary tests/test_simulation_inverted_axis_softstart_boundary.cpp "smoke;ethercat;simulation" bigcat_motion bigcat_ethercat)
    bigcat_add_optional_test(test_simulation_pose6d_softstart_completes tests/test_simulation_pose6d_softstart_completes.cpp "smoke;ethercat;simulation" bigcat_motion bigcat_ethercat)
    bigcat_add_optional_test(test_simulation_filter_velocity_stability tests/test_simulation_filter_velocity_stability.cpp "smoke;ethercat;simulation" bigcat_motion bigcat_ethercat)
    bigcat_add_optional_test(test_simulation_multiturn_actual_position_normalized tests/test_simulation_multiturn_actual_position_normalized.cpp "smoke;ethercat;simulation" bigcat_motion bigcat_ethercat)
    bigcat_add_optional_test(test_simulation_virtual_axis_pid_response tests/test_simulation_virtual_axis_pid_response.cpp "smoke;ethercat;simulation" bigcat_motion bigcat_ethercat)
    bigcat_add_optional_test(test_physical_ethercat_policy tests/test_physical_ethercat_policy.cpp "smoke;ethercat;simulation;policy" bigcat_motion bigcat_ethercat)
    bigcat_add_optional_test(test_pdofilter_velocity_continuity tests/test_pdofilter_velocity_continuity.cpp "smoke;pdofilter" bigcat_ethercat)
    bigcat_add_optional_test(test_pdofilter_random_sine_velocity_stability tests/test_pdofilter_random_sine_velocity_stability.cpp "smoke;pdofilter" bigcat_ethercat)
    bigcat_add_optional_test(test_pdofilter_pdo_limit_velocity_decay tests/test_pdofilter_pdo_limit_velocity_decay.cpp "smoke;pdofilter" bigcat_ethercat)
    bigcat_add_optional_test(test_pdofilter_violation_no_runaway tests/test_pdofilter_violation_no_runaway.cpp "smoke;pdofilter" bigcat_ethercat)
    bigcat_add_optional_test(test_pdofilter_oscillating_input_within_limits tests/test_pdofilter_oscillating_input_within_limits.cpp "smoke;pdofilter" bigcat_ethercat)
endif()

# --- 新增：在每次 Build 後自動複製 Par 資料夾到執行檔輸出目錄 -----------------
# This copies the whole directory ${CMAKE_SOURCE_DIR}/Par into the runtime output
# directory for the target (so it will appear next to the executable as a `Par` folder).
if(EXISTS "${CMAKE_SOURCE_DIR}/Par")
    message(STATUS "Par folder found. Will copy Par/ to runtime output after build.")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/Par"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/Par"
        COMMENT "Copying Par/ to runtime output directory"
    )
else()
    message(WARNING "Par directory not found at ${CMAKE_SOURCE_DIR}/Par - skipping automatic copy.")
endif()

# --- 新增：在每次 Build 後自動複製 esi 資料夾到執行檔輸出目錄 -----------------
# This copies the whole directory ${CMAKE_SOURCE_DIR}/esi into the runtime output
# directory for the target (so it will appear next to the executable as a `esi` folder).
if(EXISTS "${CMAKE_SOURCE_DIR}/esi")
    message(STATUS "esi folder found. Will copy esi/ to runtime output after build.")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/esi"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/esi"
        COMMENT "Copying esi/ to runtime output directory"
    )
else()
    message(WARNING "esi directory not found at ${CMAKE_SOURCE_DIR}/esi - skipping automatic copy.")
endif()
