// Simple header exposing the embedded HTTP server API used by MainWindow
#pragma once
#include <string>
// Reduce windows.h footprint and avoid min/max macro pollution
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <windows.h>

#include "StewartIK.h"

// Start/stop the local HTTP server. hwnd is the target HWND used for synchronous
// UI queries (status, mailbox) and to post messages back to the UI thread.
void StartLocalHttpServer(int port, HWND hwnd);
void StopLocalHttpServer();

// SSE broadcaster control (optional)
void StartSseBroadcaster();
void StopSseBroadcaster();

// Send a JSON payload to any connected SSE clients
void BroadcastSSE(const std::string &json);

// Stewart native planner helpers. These operate independently from the EtherCAT
// cyclic path and are intended for the web Stewart page's separate 2ms planner.
void SubmitStewartNativeTarget(
	uint64_t seq,
	const WuSOEM::StewartParams& params,
	const WuSOEM::Pose6D& targetPose,
	long double linearSpeedMmPerSec,
	long double angularSpeedDegPerSec,
	bool snapToTarget);
void HoldStewartNativeTarget();
std::string GetStewartNativePlannerStateJson(bool includeEventType);
void StartStewartNativePlanner();
void StopStewartNativePlanner();

// Thread-safe slave JSON cache helpers used by UI to publish slave info
void SetSlaveCacheJson(const std::string &json);
std::string GetSlaveCacheJson();

// Per-slave SDO JSON cache helpers. Win32 UI can call SetSlaveSdoCacheJson
// to publish structured SDO metadata/values for a given slave index.
void SetSlaveSdoCacheJson(int slave, const std::string &json);
std::string GetSlaveSdoCacheJson(int slave);

// Mailbox and status helpers (thread-safe). Mailbox is set by HTTP /send_mailbox
// or by posted commands and read by the UI when needed. Status is updated by UI
// and readable by the HTTP server for /status endpoint.
void SetMailboxFromHttp(const std::string &utf8Body);
std::string GetMailboxUtf8();
std::string GetLastStatusUtf8();
// Setter for last status text (thread-safe). Allows UI to push its status/log
// buffer into the HTTP server cache so the /status endpoint returns current UI logs.
void SetLastStatusUtf8(const std::string &utf8);

// Clear all active session tokens (forces logout for all clients). Thread-safe.
void ClearAllSessions();

// Set force logout flag (called when PC opens browser to force logout on next page load).
void SetForceLogoutFlag();

// Check and clear force logout flag. Returns true if logout should be forced, false otherwise.
bool CheckAndClearForceLogoutFlag();

// Small single-file cache to improve remote browsing of large logs. When a
// UI action loads a historical log, it can push the file contents into the
// HTTP server cache to avoid repeated disk reads for the same file.
// By default the server will keep at most STATUS_FILE_CACHE_MAX bytes in
// memory for the cached file.
// NOTE: no per-file server cache in this configuration — logs are read on-demand by the UI.

// Try posting a UTF-8 command string to the UI thread; if no UI target exists
// this helper will update the server cache so the web UI still sees an effect.
// Common command examples: "HTTP_CMD:CONNECT", "HTTP_CMD:DISCONNECT"
void PostCommandToUIOrCache(const std::string &cmd);

// UI state helpers: allow UI or server to set which web controls are disabled.
void SetUiDisabled(const std::string &name, bool disabled);
std::string GetUiStateJson();

// Set a short adapter status string that will be included in UI state JSON
// Format: "link_up:<ifname>" or "no_link:<ifname>". Calling this will
// broadcast an updated UI state to connected SSE clients if it changes.
void SetAdapterStatus(const std::string &s);

// Mirror cycle timing info (microseconds) from UI/EtherCAT master to HTTP server
void SetCycleInfo(uint32_t desiredUs, uint32_t commsUs, uint32_t actualUs);
// Mirror UI max observed cycle time (microseconds)
void SetMaxObservedCycleTime(uint32_t maxUs);

// Validate a provided API key string against the server's configured key
// Returns true if the key matches the server-side key (whitespace tolerant).
bool IsValidApiKey(const std::string &key);

// Return an http:// URL for the running local server, or an empty string if
// the server is not running. The returned URL will contain the bind address
// (or localhost if bound to 0.0.0.0) and port, e.g. "http://192.168.0.5:8080/".
std::string GetHttpServerUrl();

// Connection count helpers (thread-safe)
int GetActiveHttpConnections();
int GetMaxHttpConnections();
// Return number of active authenticated sessions (session store size)
int GetActiveSessionCount();
