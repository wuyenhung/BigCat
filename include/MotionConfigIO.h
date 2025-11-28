// MotionConfigIO.h
#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <map>

namespace MotionConfigIO {
    struct MotionCacheLoadDiagnostics {
        bool usedDefaultTemplate = false;
        bool repairedInvalidValues = false;
        bool rewroteTargetFile = false;
        std::vector<std::string> messages;
    };

    // Save the contents of a ListView containing motion parameters to an XML file.
    // Parameters:
    //  - hListMotionParams: HWND of the ListView (expected to be a report-style ListView)
    //  - slaveIndex: zero-based index of selected slave, or -1 when none selected
    //  - outSavedFilenameUtf8: output parameter receiving the saved filename (UTF-8)
    // Returns true on success.
    bool SaveMotionParamsToFile(HWND hListMotionParams, int slaveIndex, std::string &outSavedFilenameUtf8);
    // Save to an explicit path (wide string). This does not prompt UI.
    bool SaveMotionParamsToFileAtPath(HWND hListMotionParams, const std::wstring &filenameW, std::string &outSavedFilenameUtf8);
    // Load motion parameters from MT*.xml into the provided ListView.
    // If slaveIndex >= 0, callers may expect MT-<n>.xml to be preferred.
    // outLoadedFilenameUtf8 receives the actual filename loaded (UTF-8) on success.
    bool LoadMotionParamsFromFile(HWND hListMotionParams, int slaveIndex, std::string &outLoadedFilenameUtf8);
    // Load from an explicit path (wide string). This does not prompt UI.
    bool LoadMotionParamsFromFileAtPath(HWND hListMotionParams, const std::wstring &filenameW, std::string &outLoadedFilenameUtf8);

    // New: Save the entire in-memory motion parameters cache to an XML file.
    // cache: map of slave index (1-based) -> rows (each row has 6 columns: Index/Name/DataType/Value/Unit/Desc)
    // filenameW: output file path (e.g., L"motion.xml")
    // outSavedFilenameUtf8: receives the saved filename in UTF-8 on success
    bool SaveMotionCacheToFileAtPath(const std::map<int, std::vector<std::vector<std::wstring>>>& cache,
                                     const std::wstring &filenameW,
                                     std::string &outSavedFilenameUtf8);

    // New: Load the entire in-memory motion parameters cache from an XML file (motion.xml format)
    // On success, 'cacheOut' is replaced with parsed data and outLoadedFilenameUtf8 gets the path.
    bool LoadMotionCacheFromFileAtPath(std::map<int, std::vector<std::vector<std::wstring>>> &cacheOut,
                                       const std::wstring &filenameW,
                                       std::string &outLoadedFilenameUtf8);

    // Load motion.xml with recovery. Missing/corrupt files fall back to motion.default.xml,
    // and unreasonable values are replaced by defaults from that template before returning.
    bool LoadMotionCacheFromFileAtPathWithDefaults(std::map<int, std::vector<std::vector<std::wstring>>> &cacheOut,
                                                   const std::wstring &filenameW,
                                                   std::string &outLoadedFilenameUtf8,
                                                   MotionCacheLoadDiagnostics* diagnostics = nullptr);
}
