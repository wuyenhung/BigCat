# TPM (Trusted Platform Module) requirement for BigCat

Overview

This application requires a platform TPM (TPM 2.0 recommended) for physical EtherCAT operation. When a system does not expose a ready TPM, startup shows a warning and continues in simulation-only mode; physical EtherCAT communication is disabled before SOEM/Npcap touches a NIC. This protects license and cooldown mechanisms that rely on TPM-protected state while still allowing built-in simulation and smoke validation.

How TPM presence and readiness are detected

The detection is a best-effort multi-path probe (Windows):

The code performs a stricter "ready" probe used at startup (IsTpmReady) which attempts several checks in order:

- TBS (TPM Base Services) query is attempted first (TPM2 GetCapability via TBS).
- NCrypt Platform Crypto Provider (MS_PLATFORM_CRYPTO_PROVIDER) and enumerating platform keys.
- WMI (Win32_Tpm) checks for properties such as TpmReady, IsEnabled or IsOwned.

If none of the above indicate a TPM that is enabled/ready, the app records the restriction reason, calls `EtherCATMaster::SetPhysicalEthercatAllowed(false, ...)`, and keeps the built-in simulation adapter available.

How to verify on your machine

PowerShell (Windows 10+):

```powershell
# requires administrative privileges if you're querying system TPM state
Get-TPM
```

If `Get-TPM` reports `TpmPresent : True` and `TpmReady : True` then TPM is present and available.

Command-line (Windows):

- Check registry/device via Device Manager (Security devices > Trusted Platform Module).
- Use WMI query: `Get-WmiObject -Namespace root\CIMV2 -Class Win32_Tpm`.

Development notes

- This check is enforced at startup for physical EtherCAT access. Non-TPM development and validation should use the built-in simulation adapter instead of adding local bypasses in `src/main.cpp`.

- The LicenseChecker component already uses TPM-related APIs to derive a device fingerprint and to support license verification. The startup `IsTpmReady()` probe is intentionally conservative and focuses on detecting TPM functionality and readiness rather than proving full TPM health.

Security considerations

- Detecting a TPM does not by itself guarantee a secure, un-tampered platform; it only shows that a TPM is available and responds.
- For strong protection of keys and secrets, consider integrating platform attestation and server-side verification.
