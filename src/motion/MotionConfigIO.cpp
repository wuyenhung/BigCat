#include "../include/MotionConfigIO.h"
#include <windows.h>
#include <commctrl.h>
#include <string>
#include <sstream>
#include "../include/core/CoreHelpers.h"
#include "../lib/tinyxml2/tinyxml2.h"
#include <cmath>
#include <filesystem>
#include <set>
#include <vector>

namespace {

using MotionCacheRows = std::vector<std::vector<std::wstring>>;
using MotionCache = std::map<int, MotionCacheRows>;

bool WriteUtf8TextFile(const std::wstring &filenameW, const std::string &contents)
{
    HANDLE h = CreateFileW(filenameW.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(h, contents.data(), (DWORD)contents.size(), &written, NULL);
    CloseHandle(h);
    return ok && written == (DWORD)contents.size();
}

bool EnsureParentDirectoryExists(const std::wstring &filenameW)
{
    std::error_code ec;
    const std::filesystem::path parent = std::filesystem::path(filenameW).parent_path();
    if (parent.empty()) return true;
    std::filesystem::create_directories(parent, ec);
    return !ec;
}

bool FileExists(const std::wstring &filenameW)
{
    const DWORD attrs = GetFileAttributesW(filenameW.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool IsFiniteDouble(double value)
{
    return std::isfinite(value);
}

bool TryParseFiniteDouble(const std::wstring &text, double &out)
{
    if (text.empty()) return false;
    wchar_t *end = nullptr;
    const double value = wcstod(text.c_str(), &end);
    if (end == text.c_str() || (end && *end != L'\0') || !IsFiniteDouble(value)) return false;
    out = value;
    return true;
}

bool TryParseLongValue(const std::wstring &text, long &out)
{
    if (text.empty()) return false;
    wchar_t *end = nullptr;
    const long value = wcstol(text.c_str(), &end, 10);
    if (end == text.c_str() || (end && *end != L'\0')) return false;
    out = value;
    return true;
}

std::wstring GetRowCell(const std::vector<std::wstring> &row, size_t index)
{
    return index < row.size() ? row[index] : std::wstring();
}

int FindRowByParamIndex(const MotionCacheRows &rows, const std::wstring &paramIndex)
{
    for (size_t i = 0; i < rows.size(); ++i) {
        if (GetRowCell(rows[i], 0) == paramIndex) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void RecordDiagnostic(MotionConfigIO::MotionCacheLoadDiagnostics *diagnostics, const std::string &message)
{
    if (diagnostics) diagnostics->messages.push_back(message);
}

bool IsPrimaryMotionCacheFile(const std::wstring &filenameW)
{
    const std::wstring filename = std::filesystem::path(filenameW).filename().wstring();
    return !_wcsicmp(filename.c_str(), L"motion.xml");
}

bool IsDefaultMotionCacheTemplate(const std::wstring &filenameW)
{
    const std::wstring filename = std::filesystem::path(filenameW).filename().wstring();
    return !_wcsicmp(filename.c_str(), L"motion.default.xml");
}

std::wstring GetAdjacentMotionDefaultTemplatePath(const std::wstring &filenameW)
{
    const std::filesystem::path requested(filenameW);
    if (!requested.has_parent_path()) return std::wstring();
    return (requested.parent_path() / L"motion.default.xml").wstring();
}

std::wstring ResolveMotionDefaultTemplatePath(const std::wstring &filenameW)
{
    const std::wstring adjacentTemplate = GetAdjacentMotionDefaultTemplatePath(filenameW);
    if (!adjacentTemplate.empty() && _wcsicmp(adjacentTemplate.c_str(), filenameW.c_str()) != 0 && FileExists(adjacentTemplate)) {
        return adjacentTemplate;
    }

    wchar_t modulePath[MAX_PATH] = {0};
    if (GetModuleFileNameW(NULL, modulePath, MAX_PATH) > 0) {
        std::filesystem::path runtimeTemplate(modulePath);
        runtimeTemplate = runtimeTemplate.parent_path() / L"Par" / L"motion.default.xml";
        const std::wstring runtimeTemplateW = runtimeTemplate.wstring();
        if (_wcsicmp(runtimeTemplateW.c_str(), filenameW.c_str()) != 0 && FileExists(runtimeTemplateW)) {
            return runtimeTemplateW;
        }
    }

    return std::wstring();
}

void SyncMotionDefaultTemplateSnapshot(const std::wstring &filenameW, const std::string &xml)
{
    if (!IsPrimaryMotionCacheFile(filenameW) || IsDefaultMotionCacheTemplate(filenameW)) return;
    const std::wstring templatePath = GetAdjacentMotionDefaultTemplatePath(filenameW);
    if (templatePath.empty() || _wcsicmp(templatePath.c_str(), filenameW.c_str()) == 0) return;
    if (!EnsureParentDirectoryExists(templatePath)) return;
    WriteUtf8TextFile(templatePath, xml);
}

bool ParseMotionCacheDocument(const tinyxml2::XMLDocument &doc, MotionCache &cacheOut)
{
    const tinyxml2::XMLElement *root = doc.FirstChildElement("MotionCache");
    if (!root) return false;

    MotionCache parsed;
    for (const tinyxml2::XMLElement *slave = root->FirstChildElement("Slave"); slave; slave = slave->NextSiblingElement("Slave")) {
        int idx = 0;
        slave->QueryIntAttribute("index", &idx);
        if (idx <= 0) continue;

        MotionCacheRows rows;
        for (const tinyxml2::XMLElement *param = slave->FirstChildElement("Param"); param; param = param->NextSiblingElement("Param")) {
            const char *cIndex = nullptr;
            const char *cName = nullptr;
            const char *cDataType = nullptr;
            const char *cValue = nullptr;
            const char *cUnit = nullptr;
            const char *cDesc = nullptr;
            const tinyxml2::XMLElement *e = nullptr;

            e = param->FirstChildElement("Index"); if (e) cIndex = e->GetText();
            e = param->FirstChildElement("Name"); if (e) cName = e->GetText();
            e = param->FirstChildElement("DataType"); if (e) cDataType = e->GetText();
            e = param->FirstChildElement("Value"); if (e) cValue = e->GetText();
            e = param->FirstChildElement("Unit"); if (e) cUnit = e->GetText();
            e = param->FirstChildElement("Desc"); if (e) cDesc = e->GetText();

            std::vector<std::wstring> row(6);
            row[0] = CoreHelpers::Utf8ToWide(cIndex ? cIndex : "");
            row[1] = CoreHelpers::Utf8ToWide(cName ? cName : "");
            row[2] = CoreHelpers::Utf8ToWide(cDataType ? cDataType : "");
            row[3] = CoreHelpers::Utf8ToWide(cValue ? cValue : "");
            row[4] = CoreHelpers::Utf8ToWide(cUnit ? cUnit : "");
            row[5] = CoreHelpers::Utf8ToWide(cDesc ? cDesc : "");
            rows.push_back(std::move(row));
        }

        parsed[idx] = std::move(rows);
    }

    cacheOut = std::move(parsed);
    return true;
}

bool LoadMotionCacheFileToMap(const std::wstring &filenameW, MotionCache &cacheOut)
{
    const std::string pathUtf8 = CoreHelpers::WideToUtf8(filenameW);
    tinyxml2::XMLDocument doc;
    const tinyxml2::XMLError err = doc.LoadFile(pathUtf8.c_str());
    if (err != tinyxml2::XML_SUCCESS) return false;
    return ParseMotionCacheDocument(doc, cacheOut);
}

bool IsReasonableMotionParamValue(const std::wstring &paramIndex, const std::wstring &value)
{
    if (paramIndex.empty()) return true;
    if (paramIndex == L"P0") return !value.empty();

    double numericValue = 0.0;
    if (paramIndex == L"P1" ||
        paramIndex == L"P3" ||
        paramIndex == L"P4" ||
        paramIndex == L"P5" ||
        paramIndex == L"P6" ||
        paramIndex == L"P7" ||
        paramIndex == L"P8") {
        return TryParseFiniteDouble(value, numericValue) && numericValue > 0.0;
    }

    if (paramIndex == L"P9" || paramIndex == L"P10" || paramIndex == L"P13") {
        return TryParseFiniteDouble(value, numericValue);
    }

    if (paramIndex == L"P11" || paramIndex == L"P12") {
        return TryParseFiniteDouble(value, numericValue) && numericValue > 0.0;
    }

    if (paramIndex == L"P2") {
        long parsed = 0;
        return TryParseLongValue(value, parsed) && (parsed == 0 || parsed == 1);
    }

    return !value.empty();
}

bool RepairPositionLimitRange(MotionCacheRows &rows,
                              const MotionCacheRows &defaultRows,
                              int slaveIndex,
                              MotionConfigIO::MotionCacheLoadDiagnostics *diagnostics)
{
    const int minRow = FindRowByParamIndex(rows, L"P9");
    const int maxRow = FindRowByParamIndex(rows, L"P10");
    if (minRow < 0 || maxRow < 0) return false;

    double minLimit = 0.0;
    double maxLimit = 0.0;
    if (!TryParseFiniteDouble(GetRowCell(rows[minRow], 3), minLimit) ||
        !TryParseFiniteDouble(GetRowCell(rows[maxRow], 3), maxLimit) ||
        minLimit < maxLimit) {
        return false;
    }

    const int defaultMinRow = FindRowByParamIndex(defaultRows, L"P9");
    const int defaultMaxRow = FindRowByParamIndex(defaultRows, L"P10");
    if (defaultMinRow < 0 || defaultMaxRow < 0) return false;

    rows[minRow] = defaultRows[defaultMinRow];
    rows[maxRow] = defaultRows[defaultMaxRow];
    RecordDiagnostic(diagnostics,
                     "slave " + std::to_string(slaveIndex) +
                     " position range invalid; restored P9/P10 from default template");
    return true;
}

bool MergeMotionCacheWithDefaults(MotionCache &cache,
                                  const MotionCache &defaults,
                                  MotionConfigIO::MotionCacheLoadDiagnostics *diagnostics)
{
    bool modified = false;

    for (const auto &entry : defaults) {
        const int slaveIndex = entry.first;
        const MotionCacheRows &defaultRows = entry.second;

        auto cacheIt = cache.find(slaveIndex);
        if (cacheIt == cache.end()) {
            cache[slaveIndex] = defaultRows;
            modified = true;
            RecordDiagnostic(diagnostics,
                             "slave " + std::to_string(slaveIndex) +
                             " missing; restored from default template");
            continue;
        }

        const MotionCacheRows &existingRows = cacheIt->second;
        MotionCacheRows mergedRows;
        mergedRows.reserve(existingRows.size() + defaultRows.size());
        std::set<std::wstring> handledIndexes;

        for (const auto &defaultRow : defaultRows) {
            const std::wstring paramIndex = GetRowCell(defaultRow, 0);
            const int existingIndex = FindRowByParamIndex(existingRows, paramIndex);
            if (existingIndex < 0) {
                mergedRows.push_back(defaultRow);
                modified = true;
                RecordDiagnostic(diagnostics,
                                 "slave " + std::to_string(slaveIndex) +
                                 " missing " + CoreHelpers::WideToUtf8(paramIndex) +
                                 "; restored from default template");
                continue;
            }

            handledIndexes.insert(paramIndex);
            const auto &existingRow = existingRows[existingIndex];
            if (IsReasonableMotionParamValue(paramIndex, GetRowCell(existingRow, 3))) {
                auto mergedRow = existingRow;
                if (mergedRow.size() < 6) mergedRow.resize(6);
                for (size_t c = 0; c < 6 && c < defaultRow.size(); ++c) {
                    if (c == 3) continue;
                    if (mergedRow[c].empty()) mergedRow[c] = defaultRow[c];
                }
                mergedRows.push_back(std::move(mergedRow));
                continue;
            }

            mergedRows.push_back(defaultRow);
            modified = true;
            RecordDiagnostic(diagnostics,
                             "slave " + std::to_string(slaveIndex) +
                             " invalid " + CoreHelpers::WideToUtf8(paramIndex) +
                             "; restored from default template");
        }

        for (const auto &existingRow : existingRows) {
            const std::wstring paramIndex = GetRowCell(existingRow, 0);
            if (!paramIndex.empty() && handledIndexes.find(paramIndex) != handledIndexes.end()) continue;
            mergedRows.push_back(existingRow);
        }

        if (RepairPositionLimitRange(mergedRows, defaultRows, slaveIndex, diagnostics)) {
            modified = true;
        }

        cacheIt->second = std::move(mergedRows);
    }

    return modified;
}

} // namespace

namespace MotionConfigIO {

// Use CoreHelpers::EscapeXml to avoid code duplication
using CoreHelpers::EscapeXml;

bool SaveMotionParamsToFile(HWND hListMotionParams, int slaveIndex, std::string &outSavedFilenameUtf8)
{
    outSavedFilenameUtf8.clear();
    if (!hListMotionParams || !IsWindow(hListMotionParams)) return false;
    std::wstring filenameW = L"MT";
    if (slaveIndex >= 0) {
        wchar_t suf[32]; swprintf_s(suf, L"-%d", slaveIndex + 1);
        filenameW += suf;
    }
    filenameW += L".xml";
    return SaveMotionParamsToFileAtPath(hListMotionParams, filenameW, outSavedFilenameUtf8);
}

bool SaveMotionParamsToFileAtPath(HWND hListMotionParams, const std::wstring &filenameW, std::string &outSavedFilenameUtf8)
{
    outSavedFilenameUtf8.clear();
    if (!hListMotionParams || !IsWindow(hListMotionParams)) return false;

    int rowCount = ListView_GetItemCount(hListMotionParams);
    const int colCount = 6;
    const char* names[colCount] = { "Index", "Name", "DataType", "Value", "Unit", "Desc" };

    std::ostringstream oss;
    oss << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    oss << "<MotionParams>\n";

    for (int r = 0; r < rowCount; ++r) {
        oss << "  <Param>\n";
        for (int c = 0; c < colCount; ++c) {
            wchar_t buf[1024]; buf[0] = L'\0';
            ListView_GetItemText(hListMotionParams, r, c, buf, (int)_countof(buf));
            std::string utf8 = CoreHelpers::WideToUtf8(std::wstring(buf));
            oss << "    <" << names[c] << ">" << EscapeXml(utf8) << "</" << names[c] << ">\n";
        }
        oss << "  </Param>\n";
    }

    oss << "</MotionParams>\n";

    std::string xml = oss.str();

    if (!WriteUtf8TextFile(filenameW, xml)) return false;
    outSavedFilenameUtf8 = CoreHelpers::WideToUtf8(filenameW);
    return true;
}

} // namespace MotionConfigIO

namespace MotionConfigIO {

bool LoadMotionParamsFromFile(HWND hListMotionParams, int slaveIndex, std::string &outLoadedFilenameUtf8)
{
    outLoadedFilenameUtf8.clear();
    // Prefer MT-<n>.xml if slaveIndex >= 0, otherwise fallback to MT.xml
    if (slaveIndex >= 0) {
        std::wstring fname = L"MT";
        wchar_t suf[32]; swprintf_s(suf, L"-%d", slaveIndex + 1);
        fname += suf;
        fname += L".xml";
        if (LoadMotionParamsFromFileAtPath(hListMotionParams, fname, outLoadedFilenameUtf8)) return true;
    }
    // fallback to MT.xml
    {
        std::wstring fname = L"MT.xml";
        if (LoadMotionParamsFromFileAtPath(hListMotionParams, fname, outLoadedFilenameUtf8)) return true;
    }
    return false;
}

bool LoadMotionParamsFromFileAtPath(HWND hListMotionParams, const std::wstring &filenameW, std::string &outLoadedFilenameUtf8)
{
    outLoadedFilenameUtf8.clear();
    if (!hListMotionParams || !IsWindow(hListMotionParams)) return false;

    // Convert wide path to UTF-8 for tinyxml2 LoadFile
    std::string pathUtf8 = CoreHelpers::WideToUtf8(filenameW);
    tinyxml2::XMLDocument doc;
    tinyxml2::XMLError err = doc.LoadFile(pathUtf8.c_str());
    if (err != tinyxml2::XML_SUCCESS) return false;

    tinyxml2::XMLElement* root = doc.FirstChildElement("MotionParams");
    if (!root) return false;

    // Clear existing items
    ListView_DeleteAllItems(hListMotionParams);

    std::vector<std::wstring> cols(6);
    int insertIndex = 0;
    for (tinyxml2::XMLElement* param = root->FirstChildElement("Param"); param; param = param->NextSiblingElement("Param")) {
        const char* cIndex = nullptr; const char* cName = nullptr; const char* cDataType = nullptr; const char* cValue = nullptr; const char* cUnit = nullptr; const char* cDesc = nullptr;
        tinyxml2::XMLElement* e;
        e = param->FirstChildElement("Index"); if (e) cIndex = e->GetText();
        e = param->FirstChildElement("Name"); if (e) cName = e->GetText();
        e = param->FirstChildElement("DataType"); if (e) cDataType = e->GetText();
        e = param->FirstChildElement("Value"); if (e) cValue = e->GetText();
        e = param->FirstChildElement("Unit"); if (e) cUnit = e->GetText();
        e = param->FirstChildElement("Desc"); if (e) cDesc = e->GetText();

        cols[0] = CoreHelpers::Utf8ToWide(cIndex ? cIndex : "");
        cols[1] = CoreHelpers::Utf8ToWide(cName ? cName : "");
        cols[2] = CoreHelpers::Utf8ToWide(cDataType ? cDataType : "");
        cols[3] = CoreHelpers::Utf8ToWide(cValue ? cValue : "");
        cols[4] = CoreHelpers::Utf8ToWide(cUnit ? cUnit : "");
        cols[5] = CoreHelpers::Utf8ToWide(cDesc ? cDesc : "");

        LVITEM lvi; ZeroMemory(&lvi, sizeof(lvi));
        lvi.mask = LVIF_TEXT;
        lvi.iItem = insertIndex;
        lvi.pszText = const_cast<LPWSTR>(cols[0].c_str());
        ListView_InsertItem(hListMotionParams, &lvi);
        // set other columns
        for (int c = 1; c < 6; ++c) {
            ListView_SetItemText(hListMotionParams, insertIndex, c, const_cast<LPWSTR>(cols[c].c_str()));
        }
        ++insertIndex;
    }

    outLoadedFilenameUtf8 = CoreHelpers::WideToUtf8(filenameW);
    return true;
}

} // namespace MotionConfigIO

namespace MotionConfigIO {

bool LoadMotionCacheFromFileAtPath(std::map<int, std::vector<std::vector<std::wstring>>> &cacheOut,
                                   const std::wstring &filenameW,
                                   std::string &outLoadedFilenameUtf8)
{
    outLoadedFilenameUtf8.clear();
    if (!LoadMotionCacheFileToMap(filenameW, cacheOut)) return false;
    outLoadedFilenameUtf8 = CoreHelpers::WideToUtf8(filenameW);
    return true;
}

bool LoadMotionCacheFromFileAtPathWithDefaults(std::map<int, std::vector<std::vector<std::wstring>>> &cacheOut,
                                               const std::wstring &filenameW,
                                               std::string &outLoadedFilenameUtf8,
                                               MotionCacheLoadDiagnostics* diagnostics)
{
    outLoadedFilenameUtf8.clear();
    if (diagnostics) *diagnostics = MotionCacheLoadDiagnostics{};

    MotionCache defaults;
    const std::wstring defaultTemplatePath = ResolveMotionDefaultTemplatePath(filenameW);
    const bool haveDefaults = !defaultTemplatePath.empty() && LoadMotionCacheFileToMap(defaultTemplatePath, defaults);

    MotionCache loadedCache;
    const bool loadedTarget = LoadMotionCacheFileToMap(filenameW, loadedCache);
    if (!loadedTarget) {
        if (!haveDefaults) return false;
        loadedCache = defaults;
        if (diagnostics) diagnostics->usedDefaultTemplate = true;
        RecordDiagnostic(diagnostics,
                         "target file missing or corrupt; restored from default template " +
                         CoreHelpers::WideToUtf8(defaultTemplatePath));
    }

    bool repairedInvalidValues = false;
    if (loadedTarget && haveDefaults) {
        repairedInvalidValues = MergeMotionCacheWithDefaults(loadedCache, defaults, diagnostics);
        if (diagnostics) diagnostics->repairedInvalidValues = repairedInvalidValues;
    }

    if ((!loadedTarget || repairedInvalidValues) && !filenameW.empty()) {
        if (EnsureParentDirectoryExists(filenameW)) {
            std::string ignoredSavedFilename;
            if (SaveMotionCacheToFileAtPath(loadedCache, filenameW, ignoredSavedFilename)) {
                if (diagnostics) diagnostics->rewroteTargetFile = true;
            }
        }
    }

    cacheOut = std::move(loadedCache);
    outLoadedFilenameUtf8 = CoreHelpers::WideToUtf8(filenameW);
    return true;
}

} // namespace MotionConfigIO

namespace MotionConfigIO {

bool SaveMotionCacheToFileAtPath(const std::map<int, std::vector<std::vector<std::wstring>>>& cache,
                                 const std::wstring &filenameW,
                                 std::string &outSavedFilenameUtf8)
{
    outSavedFilenameUtf8.clear();
    std::ostringstream oss;
    oss << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    oss << "<MotionCache>\n";
    // Each slave -> <Slave index="n"> ... rows ... </Slave>
    for (const auto &kv : cache) {
        int slave = kv.first;
        oss << "  <Slave index=\"" << slave << "\">\n";
        const auto &rows = kv.second;
        for (const auto &row : rows) {
            oss << "    <Param>\n";
            const char* names[6] = {"Index","Name","DataType","Value","Unit","Desc"};
            for (int c = 0; c < 6; ++c) {
                std::string utf8 = CoreHelpers::WideToUtf8(c < (int)row.size() ? row[c] : L"");
                oss << "      <" << names[c] << ">" << EscapeXml(utf8) << "</" << names[c] << ">\n";
            }
            oss << "    </Param>\n";
        }
        oss << "  </Slave>\n";
    }
    oss << "</MotionCache>\n";

    std::string xml = oss.str();
    if (!WriteUtf8TextFile(filenameW, xml)) return false;
    SyncMotionDefaultTemplateSnapshot(filenameW, xml);
    outSavedFilenameUtf8 = CoreHelpers::WideToUtf8(filenameW);
    return true;
}

} // namespace MotionConfigIO
