cmake_minimum_required(VERSION 3.15)
project(BigCat)

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
    
    # Then, try to find the Windows SDK automatically
    if(NOT DEFINED CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
        # Try to detect Windows SDK version
        file(GLOB SDK_VERSIONS "C:/Program Files (x86)/Windows Kits/10/Include/*")
        if(SDK_VERSIONS)
            list(SORT SDK_VERSIONS)
            list(REVERSE SDK_VERSIONS)
            list(GET SDK_VERSIONS 0 LATEST_SDK)
            get_filename_component(SDK_VERSION ${LATEST_SDK} NAME)
            set(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION ${SDK_VERSION})
            message(STATUS "Detected Windows SDK version: ${SDK_VERSION}")
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
    # Global compile options applied directory-wide (subprojects inherit these)
    add_compile_options(
        $<$<CONFIG:Release>:/O2>
        $<$<CONFIG:Release>:/GL>
        $<$<CONFIG:Release>:/arch:AVX2>
        $<$<CONFIG:Release>:/fp:fast>
    )

    # Ensure the linker is invoked with LTCG for Release executables and libraries
    # and disable incremental linking which conflicts with LTCG benefits.
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /LTCG /INCREMENTAL:NO")
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /LTCG /INCREMENTAL:NO")
    set(CMAKE_STATIC_LINKER_FLAGS_RELEASE "${CMAKE_STATIC_LINKER_FLAGS_RELEASE} /LTCG")
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
add_subdirectory(${SOEM_DIR})
# Reset for main project
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/bin/main)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/lib/main)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/lib/main)

# 專案源文件
set(PROJECT_SOURCES
    src/main.cpp
    src/EtherCATMaster.cpp
    src/Win32Window.cpp
    src/MotionController.cpp
    src/PDOHelpers.cpp
    src/StatusPane.cpp
    src/Win32Utils.cpp
    src/StatusLogger.cpp
    src/EsiParser.cpp
    resources/airwolf.rc
)

# 專案標頭檔
set(PROJECT_HEADERS
    include/EtherCATMaster.h
    include/Win32Window.h
    include/MotionController.h
    include/EsiParser.h
    include/Win32Utils.h
)

# 連結目錄（原先使用硬編碼路徑；保留 NPCAP_SDK_DIR 但 prefer find_library results below）
# ...existing code...

# 建立可執行檔
add_executable(${PROJECT_NAME} 
    ${PROJECT_SOURCES}
    ${PROJECT_HEADERS}
)

# Produce executable named BigCat.exe (keeps target name but sets output name)
set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "BigCat")

# If find_path found NPCAP headers, prefer target-scoped include directories
if (DEFINED NPCAP_INCLUDE_DIR)
    message(STATUS "Adding target-scoped NPCAP include: ${NPCAP_INCLUDE_DIR}")
    target_include_directories(${PROJECT_NAME} PRIVATE ${NPCAP_INCLUDE_DIR})
endif()

# 連結 Windows 函式庫
target_link_libraries(${PROJECT_NAME} PRIVATE
    ws2_32
    winmm
    comctl32
    setupapi
    iphlpapi
    shlwapi
    $<$<BOOL:${WPCAP_LIB}>:${WPCAP_LIB}>
    $<$<BOOL:${PACKET_LIB}>:${PACKET_LIB}>
    $<$<NOT:$<BOOL:${WPCAP_LIB}>>:wpcap>
    $<$<NOT:$<BOOL:${PACKET_LIB}>>:Packet>
)

# Link against the SOEM library built from lib/SOEM
target_link_libraries(${PROJECT_NAME} PRIVATE soem)

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

# Link tinyxml2 to project
if(USE_LOCAL_TINYXML2)
    target_link_libraries(${PROJECT_NAME} PRIVATE tinyxml2)
else()
    target_link_libraries(${PROJECT_NAME} PRIVATE tinyxml2::tinyxml2)
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
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /wd4996 /wd4819 /wd4201 /wd4244)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        _CRT_SECURE_NO_WARNINGS
        UNICODE
        _UNICODE
        WIN32
        HAVE_REMOTE
        WPCAP
    )
    # Ensure MSVC treats source files as UTF-8 so string literals in source
    # that are encoded as UTF-8 are interpreted correctly.
    target_compile_options(${PROJECT_NAME} PRIVATE /utf-8)

    # --- 高效能優化（僅對 Release 有效） -----------------------------------------
    # 使用 MSVC 的全優化與連結時最佳化（LTCG）、啟用 AVX2 指令集、快速浮點模式
    # 這些選項會提高整體效能，但可能增加編譯/連結時間並使除錯困難。
    target_compile_options(${PROJECT_NAME} PRIVATE
        $<$<CONFIG:Release>:/O2>
        $<$<CONFIG:Release>:/GL>
        $<$<CONFIG:Release>:/arch:AVX2>
        $<$<CONFIG:Release>:/fp:fast>
        $<$<CONFIG:Release>:/Ot>
    )
    target_link_options(${PROJECT_NAME} PRIVATE
        $<$<CONFIG:Release>:/LTCG>
        $<$<CONFIG:Release>:/INCREMENTAL:NO>
    )

else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra)
endif()
