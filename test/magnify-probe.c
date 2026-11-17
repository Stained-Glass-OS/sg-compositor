/*
 * magnify-probe -- a Windows program's window for magnify-gate.sh: a magenta
 * popup, 160x500 (tall: the compositor centres the test windows, and each
 * shows round the others), that stays until it is killed.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: MIT
 */
#include <windows.h>

static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmd, int show)
{
    WNDCLASSW wc = {0};
    MSG msg;

    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hbrBackground = CreateSolidBrush(RGB(255, 0, 255));
    wc.lpszClassName = L"SgMagnifyProbe";
    RegisterClassW(&wc);
    CreateWindowExW(0, wc.lpszClassName, L"magnify probe", WS_POPUP | WS_VISIBLE, 40, 40, 160, 500,
                    NULL, NULL, inst, NULL);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) DispatchMessageW(&msg);
    return 0;
}
