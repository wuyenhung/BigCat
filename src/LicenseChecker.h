// LicenseChecker.h - gather hardware IDs and verify license by SHA-256 fingerprint
#pragma once

#include <string>

namespace LicenseChecker {

// Return a device fingerprint string formed by concatenating
// BaseboardSerial + ProcessorId + TpmIdentifier (separated by '|').
// Each subfield may be empty if unavailable.
std::string GetDeviceFingerprint();

// Return a fingerprint calculated using WMI-only queries (safe for display and copy).
// This avoids relying on TPM/TBS/NCrypt and is intended for UI/debug display.
std::string GetDeviceFingerprintWmiOnly();

// Compute SHA-256 of input and return hex string (lowercase).
std::string ComputeSHA256Hex(const std::string& input);

// Verify license key passed in command-line or environment. Rules:
// - If BIGCAT_DISABLE_LICENSE_CHECK is set -> returns true.
// - If cmdline contains the literal token "wuyenhung" -> returns true (backwards compat).
// - If cmdline contains a 64-char hex string -> verify equals SHA-256(device fingerprint).
// Returns true if license accepted.
bool VerifyLicenseFromCmdLine(const std::string& cmdLine);

// Returns true if the platform has a usable TPM (TPM2 via TBS/NCrypt or
// reported by WMI). This is a best-effort probe used to gate execution on
// systems which do not expose TPM functionality.
// Best-effort availability probe. For stricter requirement (TPM must be enabled
// and ready for use) use IsTpmReady().
bool IsTpmAvailable();

// Returns true if the TPM appears to be enabled, activated and ready for use
// (for example: TPM present + TpmReady/IsEnabled/IsOwned/NCrypt provider working).
// This is a stricter gate intended to prevent execution on systems where the
// TPM has been disabled in firmware or Windows settings.
bool IsTpmReady();

} // namespace LicenseChecker
