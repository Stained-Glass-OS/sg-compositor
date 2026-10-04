/* deskcomp-gate.sh's hands: Windows windows for sg-deskcomp to composite.
 *   deskcomp-probe win TITLE X Y W H     a window with a title bar, white inside
 *   deskcomp-probe layered X Y ALPHA     a red layered popup, 200x150, ALPHA/255 opaque
 *   deskcomp-probe argb X Y              a popup with per-pixel alpha: blue, a quarter opaque
 *   deskcomp-probe minimize TITLE        minimize that window, and exit
 *   deskcomp-probe glide TITLE X Y MS    move that window to X,Y in steps over MS ms
 *                                        (as dragged), and exit
 *   deskcomp-probe frosted X Y           a red popup, 200x150, frosted at 50% (__wine_sg_acrylic, wine-sg 0745)
 *   deskcomp-probe stripes X Y SHIFT     a popup, 300x200, of vertical stripes 4 px black,
 *                                        4 px white, moved SHIFT px (what a frosted one blurs)
 *   deskcomp-probe full                  a white popup over the whole screen, on top
 *   deskcomp-probe shrink                a magenta one over the whole screen that, after
 *                                        2 s, leaves full screen: 300x200 at 300,200
 * Each stays until killed. (Built with mingw-w64; runs in wine-sg.) */
#include <windows.h>

static COLORREF fill = RGB(255, 255, 255);
static int stripes = -1;      /* stripes: their shift; -1, a plain fill */

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_PAINT)
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        HBRUSH b = CreateSolidBrush(fill);
        FillRect(dc, &ps.rcPaint, b);
        DeleteObject(b);
        if (stripes >= 0)
        {
            int x;
            for (x = 0; x < 300; x++)
                if (((x + stripes) / 4) % 2 == 0) { RECT r = { x, 0, x + 1, 200 }; FillRect(dc, &r, GetStockObject(BLACK_BRUSH)); }
        }
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_TIMER)
    {
        KillTimer(h, 1);
        SetWindowPos(h, 0, 300, 200, 300, 200, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    if (m == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcW(h, m, w, l);
}

int wmain(int argc, WCHAR **argv)
{
    WNDCLASSW wc = { 0 };
    MSG msg;
    HWND h = 0;

    wc.lpfnWndProc = proc;
    wc.lpszClassName = L"deskcomp";
    wc.hCursor = LoadCursorW(0, (const WCHAR *)IDC_ARROW);
    RegisterClassW(&wc);
    if (argc >= 3 && !lstrcmpW(argv[1], L"minimize"))
    {
        HWND w = FindWindowW(L"deskcomp", argv[2]);
        if (w) PostMessageW(w, WM_SYSCOMMAND, SC_MINIMIZE, 0);
        return w ? 0 : 1;
    }
    if (argc >= 6 && !lstrcmpW(argv[1], L"glide"))
    {
        HWND w = FindWindowW(L"deskcomp", argv[2]);
        RECT r;
        int i, steps, x1 = _wtoi(argv[3]), y1 = _wtoi(argv[4]), ms = _wtoi(argv[5]);
        if (!w || !GetWindowRect(w, &r)) return 1;
        steps = ms / 16 > 0 ? ms / 16 : 1;
        for (i = 1; i <= steps; i++)
        {
            SetWindowPos(w, 0, r.left + (x1 - r.left) * i / steps, r.top + (y1 - r.top) * i / steps, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            Sleep(16);
        }
        return 0;
    }
    if (argc >= 2 && !lstrcmpW(argv[1], L"full"))
        h = CreateWindowExW(WS_EX_TOPMOST, L"deskcomp", L"full", WS_POPUP | WS_VISIBLE, 0, 0,
                            GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), 0, 0, 0, 0);
    else if (argc >= 2 && !lstrcmpW(argv[1], L"shrink"))
    {
        fill = RGB(255, 0, 255);
        h = CreateWindowExW(WS_EX_TOPMOST, L"deskcomp", L"shrink", WS_POPUP | WS_VISIBLE, 0, 0,
                            GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), 0, 0, 0, 0);
        SetTimer(h, 1, 2000, 0);
    }
    else if (argc >= 5 && !lstrcmpW(argv[1], L"stripes"))
    {
        stripes = _wtoi(argv[4]);
        h = CreateWindowExW(WS_EX_TOOLWINDOW, L"deskcomp", L"stripes", WS_POPUP | WS_VISIBLE,
                            _wtoi(argv[2]), _wtoi(argv[3]), 300, 200, 0, 0, 0, 0);
    }
    else if (argc >= 4 && !lstrcmpW(argv[1], L"frosted"))
    {
        fill = RGB(255, 0, 0);
        h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"deskcomp", L"frosted", WS_POPUP,
                            _wtoi(argv[2]), _wtoi(argv[3]), 200, 150, 0, 0, 0, 0);
        SetPropW(h, L"__wine_sg_acrylic", (HANDLE)50);
        ShowWindow(h, SW_SHOWNOACTIVATE);
        SetWindowPos(h, 0, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    else if (argc >= 7 && !lstrcmpW(argv[1], L"win"))
        h = CreateWindowW(L"deskcomp", argv[2], WS_OVERLAPPEDWINDOW | WS_VISIBLE, _wtoi(argv[3]), _wtoi(argv[4]),
                          _wtoi(argv[5]), _wtoi(argv[6]), 0, 0, 0, 0);
    else if (argc >= 5 && !lstrcmpW(argv[1], L"layered"))
    {
        fill = RGB(255, 0, 0);
        h = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"deskcomp", L"layered", WS_POPUP,
                            _wtoi(argv[2]), _wtoi(argv[3]), 200, 150, 0, 0, 0, 0);
        SetLayeredWindowAttributes(h, 0, (BYTE)_wtoi(argv[4]), LWA_ALPHA);
        ShowWindow(h, SW_SHOWNOACTIVATE);
    }
    else if (argc >= 4 && !lstrcmpW(argv[1], L"argb"))
    {
        BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), 200, -150, 1, 32, BI_RGB } };
        DWORD *bits;
        HDC sdc = GetDC(0), mdc = CreateCompatibleDC(sdc);
        HBITMAP bmp = CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, (void **)&bits, 0, 0);
        POINT src = { 0, 0 }, pos = { _wtoi(argv[2]), _wtoi(argv[3]) };
        SIZE sz = { 200, 150 };
        BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        int i;
        h = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"deskcomp", L"argb", WS_POPUP,
                            pos.x, pos.y, 200, 150, 0, 0, 0, 0);
        for (i = 0; i < 200 * 150; i++) bits[i] = 0x40000040;   /* premultiplied: blue, a quarter */
        SelectObject(mdc, bmp);
        UpdateLayeredWindow(h, sdc, &pos, &sz, mdc, &src, 0, &bf, ULW_ALPHA);
        ShowWindow(h, SW_SHOWNOACTIVATE);
    }
    if (!h) return 2;
    while (GetMessageW(&msg, 0, 0, 0)) DispatchMessageW(&msg);
    return 0;
}
