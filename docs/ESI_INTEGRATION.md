ESI Integration - tinyxml2

Overview

This document explains how to integrate the `EsiParser` (src/EsiParser.h/.cpp) into the project, add tinyxml2 to CMake, and call the parser from `Win32Window.cpp` to populate UI controls with Objects and PDO mappings.

1) Add tinyxml2 to the project

Option A - Use system installed tinyxml2 or package manager
- Find and link tinyxml2 with `find_package(tinyxml2 REQUIRED)` and `target_link_libraries(your_target PRIVATE tinyxml2::tinyxml2)`.

Option B - Add tinyxml2 as a header/source to the tree
- Download tinyxml2.h/.cpp and place them under `lib/tinyxml2/` then add to CMake with `add_library(tinyxml2 STATIC lib/tinyxml2/tinyxml2.cpp)` and link.

Example CMake fragment (add to CMakeLists.txt):

```cmake
# if you have tinyxml2 as subfolder
add_library(tinyxml2 STATIC "${CMAKE_SOURCE_DIR}/lib/tinyxml2/tinyxml2.cpp")
target_include_directories(tinyxml2 PUBLIC "${CMAKE_SOURCE_DIR}/lib/tinyxml2")

# link it to your executable target (replace ${PROJECT_NAME} with actual target)
target_link_libraries(${PROJECT_NAME} PRIVATE tinyxml2)
```

2) How to call EsiParser from UI (Win32Window)

- Include the header:

```cpp
#include "EsiParser.h"
```

- When user selects an ESI file (from combo or browse), call:

```cpp
std::vector<EsiObjectEntry> objects;
std::vector<EsiPdoEntry> pdos;
std::string err;
if (!EsiParser::Load(selectedPathUtf8, objects, pdos, err)) {
    // show error
}
```

- Populate a ComboBox for SDOs (example, assuming `hComboSDO` exists):

```cpp
SendMessageW(hComboSDO, CB_RESETCONTENT, 0, 0);
for (const auto &o : objects) {
    char buf[128];
    snprintf(buf, sizeof(buf), "0x%04X:%02X - %s", o.index, o.subindex, o.name.c_str());
    // convert to wide and add
}
```

- Populate a ListView for PDOs and their entries (example data driven).

3) Using the selected entry to perform SDO/PDO operations

- When user selects an SDO entry, parse the text or keep a mapping from combobox index to `EsiObjectEntry`. Then call existing SDO read/write routines using `o.index` and `o.subindex`.

4) Notes and caveats

- ESI files differ between vendors; the parser is a best-effort. Complex ESI with variant structures may need more robust parsing.
- The parser extracts basic fields: Index, Subindex, Name, Description, BitLength (when available), PDO entries.
- You may want to cache parsed results per file path to avoid reparsing on frequent UI interactions.

4) Next steps

- Integrate the UI controls into `Win32Window.cpp` and call `EsiParser::Load` on selection.
- Optionally add logic to auto-select ESI based on slave vendor/product id after scanning.
