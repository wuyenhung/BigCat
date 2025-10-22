#pragma once
#include <windows.h>

// Centralized layout configuration and helper for UI placement.
struct LayoutConfig {
    static constexpr int left = 4;
    static constexpr int labelW = 130;
    static constexpr int rowH = 18;
    static constexpr int vpad = 8; // vertical padding between rows
};

class LayoutHelper {
public:
    LayoutHelper(int left, int labelW, int ctrlW, int rowH, int vpad)
        : left(left), labelW(labelW), ctrlX(left + labelW + 8), ctrlW(ctrlW), rowH(rowH), vpad(vpad)
    {
        x = left;
        y = 10;
    }

    // Move cursor to next row
    void NextRow() { y += rowH + vpad; x = left; }

    // Move cursor by precise pixels
    void MoveDown(int pixels) { y += pixels; }

    // Place label at left and return RECT for it
    RECT LabelRect() const { RECT r = { left, y, left + labelW, y + rowH }; return r; }

    // Return control X coordinate and width
    int ControlX() const { return ctrlX; }
    int ControlW() const { return ctrlW; }

    int X() const { return x; }
    int Y() const { return y; }

    // helper to compute a right column X based on control width
    int RightX() const { return ctrlX + ctrlW + 12; }

private:
    int left;
    int labelW;
    int ctrlX;
    int ctrlW;
    int rowH;
    int vpad;
    int x;
    int y;
};
