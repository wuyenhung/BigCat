#pragma once

#ifndef AUTOWINDOW_H
#define AUTOWINDOW_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>

// Creates a modeless auto window with default size 400x800 and returns its HWND.
// If creation fails, returns NULL.
HWND CreateAutoWindow(HWND hOwner);

// Destroys an auto window created by CreateAutoWindow. Safe to call with NULL.
void DestroyAutoWindow(HWND hWnd);

#endif // AUTOWINDOW_H
