// LicenseChecker.cpp - 簡要說明:
// 透過 WMI/TPM 蒐集硬體指紋並進行 SHA-256 驗證，用於授權檢查。

#include "LicenseChecker.h"

#include <windows.h>
#include <wbemidl.h>
#include <comdef.h>
#pragma comment(lib, "wbemuuid.lib")

#include <bcrypt.h>
#include <ncrypt.h>
#pragma comment(lib, "ncrypt.lib")
#pragma comment(lib, "Bcrypt.lib")
#include <tbs.h>
#pragma comment(lib, "Tbs.lib")

#include <string>
#include <vector>
#include <algorithm>
#include <sstream>
#include <iomanip>

namespace {

// Helper: run a simple WMI query and return the first matching property's value as utf8 string
static std::string QueryWmiString(const wchar_t* query, const wchar_t* propName) {
    std::string empty;

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    bool initialized = SUCCEEDED(hr);

    IWbemLocator* pLocator = nullptr;
    IWbemServices* pServices = nullptr;
    IEnumWbemClassObject* pEnum = nullptr;

    hr = CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER,
                          IID_IWbemLocator, reinterpret_cast<LPVOID*>(&pLocator));
    if (FAILED(hr) || !pLocator) goto cleanup;

    hr = pLocator->ConnectServer(_bstr_t(L"ROOT\\CIMV2"), NULL, NULL, 0, NULL, 0, 0, &pServices);
    if (FAILED(hr) || !pServices) goto cleanup;

    // Set security levels on the proxy
    CoSetProxyBlanket(pServices, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
                      RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);

    hr = pServices->ExecQuery(_bstr_t(L"WQL"), _bstr_t(query), WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &pEnum);
    if (FAILED(hr) || !pEnum) goto cleanup;

    IWbemClassObject* pObj = nullptr;
    ULONG returned = 0;
    hr = pEnum->Next(WBEM_INFINITE, 1, &pObj, &returned);
    if (hr == S_OK && returned == 1 && pObj) {
        VARIANT vtProp;
        VariantInit(&vtProp);
        hr = pObj->Get(propName, 0, &vtProp, 0, 0);
            if (SUCCEEDED(hr) && (vtProp.vt == VT_BSTR)) {
                // convert wide to utf8; copy BSTR into _bstr_t to avoid double-free issues
                _bstr_t bstr(vtProp.bstrVal);
                const char* utf8 = bstr;
                if (utf8) {
                    std::string out(utf8);
                    VariantClear(&vtProp);
                    pObj->Release();
                    // assign to empty to return via cleanup path
                    if (pEnum) pEnum->Release(); pEnum = nullptr;
                    if (pServices) pServices->Release(); pServices = nullptr;
                    if (pLocator) pLocator->Release(); pLocator = nullptr;
                    if (initialized) { CoUninitialize(); initialized = false; }
                    return out;
                }
        }
        VariantClear(&vtProp);
        pObj->Release();
    }

cleanup:
    if (pEnum) { pEnum->Release(); }
    if (pServices) pServices->Release();
    if (pLocator) { pLocator->Release(); }
    if (initialized) { CoUninitialize(); }
    return empty;
}

// Helper: simple tokenization to find a 64-char hex in a string
static bool FindHex64Token(const std::string& s, std::string& outHex) {
    for (size_t i = 0; i + 64 <= s.size(); ++i) {
        bool ok = true;
        for (size_t j = 0; j < 64; ++j) {
            char c = s[i+j];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) { ok = false; break; }
        }
        if (ok) {
            outHex = s.substr(i, 64);
            // normalize to lowercase
            std::transform(outHex.begin(), outHex.end(), outHex.begin(), ::tolower);
            return true;
        }
    }
    return false;
}

// Try TBS direct TPM2 GetCapability to obtain manufacturer/vendor strings.
static std::string QueryTpmViaTbs() {
    std::string tpm;
    TBS_CONTEXT_PARAMS2 params;
    ZeroMemory(&params, sizeof(params));
    params.version = TBS_CONTEXT_VERSION_TWO;
    params.includeTpm20 = 1;

    TBS_HCONTEXT hContext = NULL;
    TBS_RESULT tr = Tbsi_Context_Create(reinterpret_cast<PCTBS_CONTEXT_PARAMS>(&params), &hContext);
    if (tr == TBS_SUCCESS && hContext != NULL) {
        const uint16_t tag = 0x8001; // TPM2_ST_NO_SESSIONS
        const uint32_t commandCode = 0x0000017A; // TPM2_CC_GetCapability
        const uint32_t cap = 0x00000006; // TPM2_CAP_TPM_PROPERTIES
        const uint32_t property = 0x00000105; // TPM2_PT_MANUFACTURER
        const uint32_t propertyCount = 6;

        uint32_t totalSize = 2 + 4 + 4 + 4 + 4 + 4;
        std::vector<BYTE> cmd;
        auto push16 = [&](uint16_t v){ cmd.push_back((BYTE)(v >> 8)); cmd.push_back((BYTE)(v & 0xFF)); };
        auto push32 = [&](uint32_t v){ cmd.push_back((BYTE)(v >> 24)); cmd.push_back((BYTE)((v >> 16) & 0xFF)); cmd.push_back((BYTE)((v >> 8) & 0xFF)); cmd.push_back((BYTE)(v & 0xFF)); };
        push16(tag);
        push32(totalSize);
        push32(commandCode);
        push32(cap);
        push32(property);
        push32(propertyCount);

            UINT32 outLen = 4096;
            std::vector<BYTE> outBuf(outLen);
            // Tbsip_Submit_Command expects (hContext, locality, priority, pbInput, cbInput, pbOutput, pcbOutput)
            tr = Tbsip_Submit_Command(hContext, TBS_COMMAND_LOCALITY_ZERO, TBS_COMMAND_PRIORITY_NORMAL, cmd.data(), (UINT32)cmd.size(), outBuf.data(), &outLen);
        if (tr == TBS_SUCCESS && outLen > 0) {
            if (outLen >= 19) {
                size_t off = 0;
                off += 2; // tag
                off += 4; // size
                uint32_t rc = (outBuf[off] << 24) | (outBuf[off+1] << 16) | (outBuf[off+2] << 8) | outBuf[off+3]; off += 4;
                if (rc == 0) {
                    if (off + 1 <= outLen) off += 1; // moreData
                    if (off + 4 <= outLen) {
                        uint32_t respCap = (outBuf[off]<<24)|(outBuf[off+1]<<16)|(outBuf[off+2]<<8)|outBuf[off+3]; off+=4;
                        if (respCap == cap) {
                            if (off + 4 <= outLen) {
                                uint32_t count = (outBuf[off]<<24)|(outBuf[off+1]<<16)|(outBuf[off+2]<<8)|(outBuf[off+3]); off+=4;
                                for (uint32_t i=0;i<count;i++) {
                                    if (off + 8 <= outLen) {
                                        uint32_t prop = (outBuf[off]<<24)|(outBuf[off+1]<<16)|(outBuf[off+2]<<8)|(outBuf[off+3]); off+=4;
                                        uint32_t val  = (outBuf[off]<<24)|(outBuf[off+1]<<16)|(outBuf[off+2]<<8)|(outBuf[off+3]); off+=4;
                                        if (prop == 0x00000105) {
                                            char mf[5] = {0};
                                            mf[0] = (char)((val>>24)&0xFF);
                                            mf[1] = (char)((val>>16)&0xFF);
                                            mf[2] = (char)((val>>8)&0xFF);
                                            mf[3] = (char)((val)&0xFF);
                                            tpm += std::string(mf);
                                        } else if (prop >= 0x00000106 && prop <= 0x00000109) {
                                            char chunk[5] = {0};
                                            chunk[0] = (char)((val>>24)&0xFF);
                                            chunk[1] = (char)((val>>16)&0xFF);
                                            chunk[2] = (char)((val>>8)&0xFF);
                                            chunk[3] = (char)((val)&0xFF);
                                            tpm += std::string(chunk);
                                        } else {
                                            std::ostringstream oss; oss << std::hex << val;
                                            tpm += oss.str();
                                        }
                                    } else break;
                                }
                            }
                        }
                    }
                }
            }
        }
        Tbsip_Context_Close(hContext);
    }
    return tpm;
}

} // anonymous namespace

namespace LicenseChecker {

std::string GetDeviceFingerprint() {
    // Order: BaseboardSerial | ProcessorId | TPMIdentifier
    std::string baseboard = QueryWmiString(L"SELECT SerialNumber FROM Win32_BaseBoard", L"SerialNumber");
    std::string processor = QueryWmiString(L"SELECT ProcessorId FROM Win32_Processor", L"ProcessorId");
    // Prefer TPM EK public key (CNG Platform Crypto Provider) fingerprint when available
    std::string tpm = "";
    // Try TBS direct TPM2 GetCapability first
    tpm = QueryTpmViaTbs();
        NCRYPT_PROV_HANDLE hProvider = NULL;
        SECURITY_STATUS sec = NCryptOpenStorageProvider(&hProvider, MS_PLATFORM_CRYPTO_PROVIDER, 0);
            if (sec == ERROR_SUCCESS && hProvider) {
            // We'll use NCryptEnumKeys to retrieve key names
            NCryptKeyName* pName = nullptr;
            SECURITY_STATUS s2 = NCryptEnumKeys(hProvider, nullptr, &pName, nullptr, 0);
            if (s2 == ERROR_SUCCESS && pName) {
                // attempt to open the first key and get public blob
                NCRYPT_KEY_HANDLE hK = NULL;
                if (NCryptOpenKey(hProvider, &hK, pName->pszName, 0, 0) == ERROR_SUCCESS && hK) {
                    DWORD cb = 0;
                    SECURITY_STATUS s3 = NCryptExportKey(hK, NULL, BCRYPT_ECCPUBLIC_BLOB, NULL, NULL, 0, &cb, 0);
                    if (s3 != ERROR_SUCCESS) {
                        // try RSA public blob
                        s3 = NCryptExportKey(hK, NULL, BCRYPT_RSAPUBLIC_BLOB, NULL, NULL, 0, &cb, 0);
                    }
                    if (s3 == ERROR_SUCCESS && cb > 0) {
                        std::vector<BYTE> blob(cb);
                        if (NCryptExportKey(hK, NULL, BCRYPT_ECCPUBLIC_BLOB, NULL, blob.data(), cb, &cb, 0) != ERROR_SUCCESS) {
                            // try RSA export if ECC failed
                            if (NCryptExportKey(hK, NULL, BCRYPT_RSAPUBLIC_BLOB, NULL, blob.data(), cb, &cb, 0) != ERROR_SUCCESS) {
                                // failed to export
                            } else {
                                tpm = ComputeSHA256Hex(std::string(reinterpret_cast<char*>(blob.data()), blob.size()));
                            }
                        } else {
                            tpm = ComputeSHA256Hex(std::string(reinterpret_cast<char*>(blob.data()), blob.size()));
                        }
                    }
                    NCryptFreeObject(hK);
                }
                NCryptFreeBuffer((PVOID)pName);
            }
            NCryptFreeObject(hProvider);
        }

    // fallback: WMI
    if (tpm.empty()) {
        tpm = QueryWmiString(L"SELECT * FROM Win32_Tpm", L"ManufacturerID");
        if (tpm.empty()) {
            tpm = QueryWmiString(L"SELECT * FROM Win32_Tpm", L"SpecVersion");
        }
    }

    // fallback: if ManufacturerID empty, try SpecVersion
    if (tpm.empty()) {
        tpm = QueryWmiString(L"SELECT * FROM Win32_Tpm", L"SpecVersion");
    }

    std::string joined = baseboard + "|" + processor + "|" + tpm;
    return joined;
}

std::string GetDeviceFingerprintWmiOnly() {
    // Compose a device fingerprint using only WMI queries. This is safer for UI display
    // since it doesn't touch TPM (TBS) or NCrypt paths which may not be available.
    std::string baseboard = QueryWmiString(L"SELECT SerialNumber FROM Win32_BaseBoard", L"SerialNumber");
    std::string processor = QueryWmiString(L"SELECT ProcessorId FROM Win32_Processor", L"ProcessorId");
    std::string tpm = QueryWmiString(L"SELECT * FROM Win32_Tpm", L"ManufacturerID");
    if (tpm.empty()) tpm = QueryWmiString(L"SELECT * FROM Win32_Tpm", L"SpecVersion");
    return baseboard + "|" + processor + "|" + tpm;
}

std::string ComputeSHA256Hex(const std::string& input) {
    BCRYPT_ALG_HANDLE hAlg = NULL;
    BCRYPT_HASH_HANDLE hHash = NULL;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, NULL, 0);
    if (!BCRYPT_SUCCESS(status)) {
        return std::string();
    }

    DWORD cbHashObject = 0, cbData = 0;
    BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&cbHashObject, sizeof(cbHashObject), &cbData, 0);

    std::vector<BYTE> pbHashObject(cbHashObject);
    status = BCryptCreateHash(hAlg, &hHash, pbHashObject.data(), (ULONG)pbHashObject.size(), NULL, 0, 0);
    if (!BCRYPT_SUCCESS(status)) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return std::string();
    }

    status = BCryptHashData(hHash, (PUCHAR)input.data(), (ULONG)input.size(), 0);
    if (!BCRYPT_SUCCESS(status)) {
        BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return std::string();
    }

    // SHA-256 size = 32 bytes
    std::vector<BYTE> hash(32);
    status = BCryptFinishHash(hHash, hash.data(), (ULONG)hash.size(), 0);
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    if (!BCRYPT_SUCCESS(status)) return std::string();

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (BYTE b : hash) {
        oss << std::setw(2) << static_cast<int>(b);
    }
    return oss.str();
}

bool VerifyLicenseFromCmdLine(const std::string& cmdLine) {
    // Developer bypass (only allowed in Debug builds) - **removed in Release** to require real license
    char buf[8] = {0};
    DWORD len = 0;
#if defined(_DEBUG)
    len = GetEnvironmentVariableA("BIGCAT_DISABLE_LICENSE_CHECK", buf, sizeof(buf));
    if (len > 0 && (_stricmp(buf, "1") == 0 || _stricmp(buf, "true") == 0)) {
        return true;
    }
#endif

    // If command-line contains a 64-char hex string use that as license candidate
    std::string candidate;
    if (FindHex64Token(cmdLine, candidate)) {
        std::string fingerprint = GetDeviceFingerprint();
        if (fingerprint.empty()) return false; // can't form fingerprint
        std::string computed = ComputeSHA256Hex(fingerprint);
        // normalize
        std::transform(computed.begin(), computed.end(), computed.begin(), ::tolower);
        return candidate == computed;
    }

    // Also check environment variable override (BIGCAT_LICENSE_KEY)
    char outKey[128];
    len = GetEnvironmentVariableA("BIGCAT_LICENSE_KEY", outKey, sizeof(outKey));
    if (len > 0) {
        std::string envKey(outKey);
        std::transform(envKey.begin(), envKey.end(), envKey.begin(), ::tolower);
        if (envKey.size() == 64) {
            std::string fingerprint = GetDeviceFingerprint();
            if (fingerprint.empty()) return false;
            std::string computed = ComputeSHA256Hex(fingerprint);
            std::transform(computed.begin(), computed.end(), computed.begin(), ::tolower);
            return envKey == computed;
        }
    }

    return false;
}

bool IsTpmAvailable() {
    // Quick probe order (best-effort): TBS -> NCrypt platform provider -> WMI
    // If any of these indicate TPM presence we treat it as available.
    try {
        std::string viaTbs = QueryTpmViaTbs();
        if (!viaTbs.empty()) return true;

        // NCrypt platform crypto provider present typically means TPM available
        NCRYPT_PROV_HANDLE hProvider = NULL;
        SECURITY_STATUS sec = NCryptOpenStorageProvider(&hProvider, MS_PLATFORM_CRYPTO_PROVIDER, 0);
        if (sec == ERROR_SUCCESS && hProvider) {
            NCryptFreeObject(hProvider);
            return true;
        }

        // WMI check for Win32_Tpm
        std::string wmi = QueryWmiString(L"SELECT * FROM Win32_Tpm", L"ManufacturerID");
        if (!wmi.empty()) return true;
        wmi = QueryWmiString(L"SELECT * FROM Win32_Tpm", L"SpecVersion");
        if (!wmi.empty()) return true;
    } catch (...) {
        // swallow exceptions — we perform a best-effort probe and return false if anything fails
    }
    return false;
}

bool IsTpmReady() {
    // Stricter checks to ensure TPM is enabled and ready.
    // 1) TBS query should succeed and return meaningful data
    try {
        std::string tbs = QueryTpmViaTbs();
        if (tbs.empty()) {
            // no TPM data from TBS - might be disabled
        } else {
            return true;
        }

        // 2) NCrypt platform provider should be present and at least one key should be enumeratable
        NCRYPT_PROV_HANDLE hProvider = NULL;
        SECURITY_STATUS sec = NCryptOpenStorageProvider(&hProvider, MS_PLATFORM_CRYPTO_PROVIDER, 0);
        if (sec == ERROR_SUCCESS && hProvider) {
            NCryptKeyName* pName = nullptr;
            SECURITY_STATUS s2 = NCryptEnumKeys(hProvider, nullptr, &pName, nullptr, 0);
            if (s2 == ERROR_SUCCESS && pName) {
                // There's at least one platform key name exposed by provider
                NCryptFreeBuffer((PVOID)pName);
                NCryptFreeObject(hProvider);
                return true;
            }
            if (hProvider) NCryptFreeObject(hProvider);
        }

        // 3) WMI TpmReady / IsEnabled / IsOwned
        std::string ready = QueryWmiString(L"SELECT * FROM Win32_Tpm", L"TpmReady");
        if (!ready.empty()) {
            std::string lowered = ready;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(), ::tolower);
            if (lowered.find("true") != std::string::npos) return true;
        }

        // Try alternate properties
        std::string enabled = QueryWmiString(L"SELECT * FROM Win32_Tpm", L"IsEnabled");
        if (!enabled.empty()) {
            std::string lowered = enabled;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(), ::tolower);
            if (lowered.find("true") != std::string::npos) return true;
        }

        std::string owned = QueryWmiString(L"SELECT * FROM Win32_Tpm", L"IsOwned");
        if (!owned.empty()) {
            std::string lowered = owned;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(), ::tolower);
            if (lowered.find("true") != std::string::npos) return true;
        }
    } catch (...) {
        // swallow exceptions - best-effort only
    }

    return false;
}

} // namespace LicenseChecker
