// Non-UI core helpers for MainWindow
#pragma once
#include <string>
#include <vector>
#include <windows.h>

namespace Win32WindowCore {
    // Escape a JSON string (basic escaping for control chars, quotes, backslash)
    std::string EscapeJsonString(const std::string &s);

    // Distribute an integer width into N column widths with the remainder
    // added to the last column.
    std::vector<int> DistributeColumns(int totalWidth, int colCount);


    // Set process high priority and lock process affinity to Core 4 (or best core > 0)
    // to shield from system interrupts on Core 0.
    void SetProcessHighPriorityAndCore4Affinity();
}
