#ifndef UI_CONSTANTS_H
#define UI_CONSTANTS_H

// Centralized custom window messages (WM_USER + offset).
// Keep values stable to preserve cross-module message contracts.

// Worker -> UI: UTF-8 log/event payload.
#define WM_USER_POST_UTF8_LOG (WM_USER + 101)
// Worker -> UI: one SDO item result.
#define WM_USER_SDO_ITEM_READ (WM_USER + 110)
// Request in-place edit start for SDO list.
#define WM_USER_BEGIN_VALUE_EDIT (WM_USER + 120)
// Request in-place edit start for motion list.
#define WM_USER_BEGIN_MOTION_VALUE_EDIT (WM_USER + 121)

// AutoWindow <-> MainWindow workflow.
#define WM_USER_AUTO_FAULT_RESET_REQUEST (WM_USER + 200)
#define WM_USER_AUTO_FAULT_RESET_DONE (WM_USER + 201)
#define WM_USER_AUTO_IMMEDIATE_STOP_REQUEST (WM_USER + 202)
#define WM_USER_AUTO_IMMEDIATE_STOP_DONE (WM_USER + 203)
#define WM_USER_AUTO_DOWN_TO_ORIGIN_REQUEST (WM_USER + 204)
#define WM_USER_AUTO_DOWN_TO_ORIGIN_DONE (WM_USER + 205)
#define WM_USER_PDO_PREPARE_FOR_SLAVE (WM_USER + 206)
#define WM_USER_AUTO_EMERGENCY_STOP_REQUEST (WM_USER + 207)
#define WM_USER_AUTO_EMERGENCY_STOP_DONE (WM_USER + 208)
#define WM_USER_AUTO_STATUS_UPDATE (WM_USER + 210)
#define WM_USER_AUTO_ENABLE_ALL (WM_USER + 211)
#define WM_USER_AUTO_DISABLE_ALL (WM_USER + 212)
#define WM_USER_AUTO_SET_OWNER_STATE (WM_USER + 213)
#define WM_USER_AUTO_REPORT_UI_STATE (WM_USER + 214)
#define WM_USER_AUTO_SOFT_START_SUCCEEDED (WM_USER + 215)
#define WM_USER_REFRESH_IRQ_DISPLAY (WM_USER + 216)
#define WM_USER_CLEANUP_COMPLETE (WM_USER + 217)
// Worker (HTTP/SSE) -> UI: re-apply the current adapter selection to the native
// controls (combobox/connect button/IRQ display). Native controls and registry
// work must only run on the UI thread; see repo-facts 2026-10-03.
#define WM_USER_APPLY_SELECTED_ADAPTER_UI (WM_USER + 218)

// WM_USER_AUTO_IMMEDIATE_STOP_REQUEST lParam flags.
#define AUTO_IMMEDIATE_STOP_FLAG_PDO_ERROR 0x1
#define AUTO_IMMEDIATE_STOP_FLAG_REQUIRE_MANUAL_RESET 0x2
#define AUTO_IMMEDIATE_STOP_FLAG_SKIP_MANUAL_RESET 0x4

// Batched status repaint/update signal.
#define WM_USER_STATUS_UPDATE_BATCH (WM_USER + 305)
// App-level async actions.
#define WM_USER_REQUEST_DISCONNECT (WM_USER + 400)
#define WM_USER_RESTORE_WINDOW (WM_USER + 401)

/* -------------------------------------------------------------------------
 * UI control IDs (IDC_*)
 * ---------------------------------------------------------------------- */

// Adapter / connection.
#define IDC_EDIT_ADAPTER 1001
#define IDC_BTN_CONNECT 1002
#define IDC_BTN_DISCONNECT 1003

// Slave list / status.
#define IDC_LIST_SLAVES 1004
#define IDC_EDIT_STATUS 1005

// SDO controls.
#define IDC_EDIT_SDO_INDEX 1006
#define IDC_EDIT_SDO_SUBINDEX 1007
#define IDC_EDIT_SDO_DATA 1008
#define IDC_BTN_READ_SDO 1009
#define IDC_BTN_WRITE_SDO 1010

// Mailbox.
#define IDC_EDIT_MAILBOX 1011
#define IDC_BTN_SEND_MAILBOX 1012

// Cycle / diagnostics.
#define IDC_EDIT_CYCLE_TIME 1013
#define IDC_EDIT_CURRENT_CYCLE_TIME 1014
#define IDC_EDIT_WKC 1015

// ESI.
#define IDC_BTN_BROWSE_ESI 1016
#define IDC_COMBO_SDO 1017

// PDO / IO map.
#define IDC_CHECK_PDO_ENABLE 1020
#define IDC_BTN_PDO_START 1021
#define IDC_BTN_PDO_STOP 1022
#define IDC_EDIT_IOMAP_DISPLAY 1023

// Extra SDO/parameter actions.
#define IDC_BTN_READ_ALL_SDO 1031
#define IDC_BTN_SAVE_PARAMS 1035
#define IDC_STATIC_MAILBOX 1040
#define IDC_BTN_PDO_SENT_STATUS 1050
#define IDC_BTN_IMPORT_PARAMS 1060
#define IDC_BTN_WRITE_ALL_SDO 1061

// Labels used by cycle row layout.
#define IDC_STATIC_CYCLE_ACTUAL_LABEL 1080
#define IDC_STATIC_CYCLE_WKC_LABEL 1081
#define IDC_STATIC_PDO_SENT_LABEL 1082
#define IDC_STATIC_PDO_PROC_LABEL 1083
#define IDC_EDIT_PDO_PROC_FREQ 1084

// Motion controls.
#define IDC_STATIC_MOTION_CONTROL 1100
#define IDC_BTN_FOLLOW_MODE 1101
#define IDC_BTN_POWER_ON 1102
#define IDC_BTN_POWER_OFF 1103
#define IDC_BTN_HOME 1104
#define IDC_BTN_JOG_CW 1105
#define IDC_BTN_JOG_CCW 1106
#define IDC_BTN_FAULT_RESET 1107
#define IDC_BTN_IMMEDIATE_STOP 1108
#define IDC_BTN_MANUAL_MODE 1110
#define IDC_BTN_AUTO_MODE 1111
#define IDC_BTN_LOAD_CONFIG 1112
#define IDC_BTN_SAVE_CONFIG 1113
#define IDC_CHK_MMF_OUTPUT 1114
#define IDC_CHK_HTTP_SERVER 1115
#define IDC_CHK_STARTUP_SHOWMAIN 1116
#define IDC_CHK_STARTUP_AUTOSEQ 1117
#define IDC_CHK_STATUS_LOG 1118
#define IDC_CHK_LOW_LATENCY_MMF 1120
#define IDC_CHK_MMF_POSE_OUTPUT 1121

// Adapter IRQ display
#define IDC_STATIC_ADAPTER_IRQ 1200
#define IDC_STATIC_OTHER_IRQS 1201
#define IDC_STATIC_RT_CORE   1202

// Auto window controls.
#define IDC_LIST_MMF 2070
#define IDC_BTN_MMF1_ENABLE 2071
#define IDC_BTN_MMF1_DISABLE 2072
#define IDC_STATIC_MMF 2073

#define IDC_BTN_AUTO_WINDOW 2085
#define IDC_BTN_AUTO_CONNECT 2086
#define IDC_BTN_AUTO_HOME 2087
#define IDC_BTN_AUTO_FAULT_RESET 2088
#define IDC_BTN_AUTO_IMMEDIATE_STOP 2089
#define IDC_BTN_AUTO_EMERGENCY_STOP 2090
#define IDC_BTN_AUTO_STATUS_WINDOW 2091

// Auto window status lamps.
#define IDC_AUTO_STATUS_OETHERCATOK 2100
#define IDC_AUTO_STATUS_OBUSY 2101
#define IDC_AUTO_STATUS_OERROR 2102
#define IDC_AUTO_STATUS_OREADY 2103
#define IDC_AUTO_STATUS_OHOMEDONE 2104
#define IDC_AUTO_STATUS_OONLINE 2105

#endif  // UI_CONSTANTS_H
