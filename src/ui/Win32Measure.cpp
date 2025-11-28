#include <windows.h>
#include "LayoutHelper.h"

// Measure the vertical height (pixels) of a single text row for the provided HFONT.
// Returns a sensible default (LayoutConfig::rowH) when measurement is not possible.
int MeasureTextHeight(HFONT hFont) {
    int rowHeight = LayoutConfig::rowH;
    if (hFont) {
        HDC hdcScreen = GetDC(NULL);
        if (hdcScreen) {
            HFONT oldF = (HFONT)SelectObject(hdcScreen, hFont);
            TEXTMETRIC tm; ZeroMemory(&tm, sizeof(tm));
            if (GetTextMetrics(hdcScreen, &tm)) rowHeight = tm.tmHeight + tm.tmExternalLeading;
            SelectObject(hdcScreen, oldF);
            ReleaseDC(NULL, hdcScreen);
        }
    }
    return rowHeight;
}
