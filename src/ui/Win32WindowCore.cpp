#include "../include/Win32WindowCore.h"
#include "../include/core/CoreHelpers.h"
#include <string>
#include <vector>

namespace Win32WindowCore {

// Forward implementations to CoreHelpers (migrated to src/core)
std::string EscapeJsonString(const std::string &s) { return CoreHelpers::EscapeJsonString(s); }
std::vector<int> DistributeColumns(int totalWidth, int colCount) { return CoreHelpers::DistributeColumns(totalWidth, colCount); }
// SetProcessHighPriorityAndCore4Affinity: forward to CoreHelpers
void SetProcessHighPriorityAndCore4Affinity() { CoreHelpers::SetProcessHighPriorityAndCore4Affinity(); }

} // namespace
