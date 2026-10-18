/*
 * popup-probe -- a Windows program with a combo box, for popup-gate.sh.
 * Its list is an override-redirect X window (Setup runs without a Wine
 * desktop). Prints "COMBO x y w h", then "LIST x y w h" while the list is
 * open, "SEL n" when a choice is made. Then a popup menu, as the login
 * screen's power button opens (TrackPopupMenu, TPM_RETURNCMD): "MENUBTN x y w
 * h", "MENU x y w h" while it is open, "CMD n" for the item clicked (Restart
 * is 202); exits after 60 s or on "CMD".
 *
 * Copyright (C) 2026 David Hamner and the Stained Glass OS contributors
 * SPDX-License-Identifier: MIT
 */
#include <windows.h>
#include <stdio.h>

static HWND menubtn;
static int menu_shown;

static LRESULT CALLBACK owner_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_ENTERIDLE && wp == MSGF_MENU && !menu_shown) {
		HWND m = FindWindowW(L"#32768", NULL);
		RECT r;
		if (m && GetWindowRect(m, &r)) {
			menu_shown = 1;
			printf("MENU %ld %ld %ld %ld\n", r.left, r.top, r.right - r.left, r.bottom - r.top);
			fflush(stdout);
		}
	}
	if (msg == WM_COMMAND && (HWND)lp == menubtn) {
		HMENU menu = CreatePopupMenu();
		RECT r;
		int cmd;
		AppendMenuA(menu, MF_STRING, 201, "Shut down");
		AppendMenuA(menu, MF_STRING, 202, "Restart");
		GetWindowRect(menubtn, &r);
		menu_shown = 0;
		cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_BOTTOMALIGN | TPM_NONOTIFY,
				     r.right, r.top - 4, 0, h, NULL);
		DestroyMenu(menu);
		printf("CMD %d\n", cmd);
		fflush(stdout);
		if (cmd) PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(h, msg, wp, lp);
}

static int menu_part(void)
{
	WNDCLASSW wc = { 0 };
	HWND o;
	RECT r;
	MSG m;
	int i;
	wc.lpfnWndProc = owner_proc;
	wc.lpszClassName = L"MenuOwner";
	wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	RegisterClassW(&wc);
	o = CreateWindowW(L"MenuOwner", L"menu probe", WS_POPUP | WS_VISIBLE, 0, 0, 600, 400, 0, 0, 0, 0);
	menubtn = CreateWindowW(L"BUTTON", L"Power", WS_CHILD | WS_VISIBLE, 500, 340, 80, 40, o, 0, 0, 0);
	SetForegroundWindow(o);
	for (i = 0; i < 100; i++) {
		while (PeekMessageW(&m, 0, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
		Sleep(10);
	}
	GetWindowRect(menubtn, &r);
	printf("MENUBTN %ld %ld %ld %ld\n", r.left, r.top, r.right - r.left, r.bottom - r.top);
	fflush(stdout);
	while (GetMessageW(&m, 0, 0, 0) > 0) DispatchMessageW(&m);
	return 0;
}

int main(void)
{
	HWND w = CreateWindowW(L"STATIC", L"popup probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 600, 400, 0, 0, 0, 0);
	HWND c = CreateWindowW(L"COMBOBOX", 0, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
			       40, 40, 240, 300, w, 0, 0, 0);
	COMBOBOXINFO info = { sizeof(info) };
	RECT r;
	MSG m;
	int i, dropped = 0, sel;
	DWORD start = GetTickCount();

	for (i = 0; i < 12; i++) {
		WCHAR s[16];
		wsprintfW(s, L"item %d", i);
		SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)s);
	}
	SendMessageW(c, CB_SETCURSEL, 0, 0);
	for (i = 0; i < 100; i++) {	/* settle: shown and placed */
		while (PeekMessageW(&m, 0, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
		Sleep(10);
	}
	GetWindowRect(c, &r);
	printf("COMBO %ld %ld %ld %ld\n", r.left, r.top, r.right - r.left, r.bottom - r.top);
	fflush(stdout);
	while (GetTickCount() - start < 60000) {
		while (PeekMessageW(&m, 0, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
		if (SendMessageW(c, CB_GETDROPPEDSTATE, 0, 0) && !dropped) {
			dropped = 1;
			GetComboBoxInfo(c, &info);
			GetWindowRect(info.hwndList, &r);
			printf("LIST %ld %ld %ld %ld\n", r.left, r.top, r.right - r.left, r.bottom - r.top);
			fflush(stdout);
		} else if (!SendMessageW(c, CB_GETDROPPEDSTATE, 0, 0)) {
			dropped = 0;
		}
		if ((sel = SendMessageW(c, CB_GETCURSEL, 0, 0)) > 0) {
			printf("SEL %d\n", sel);
			fflush(stdout);
			DestroyWindow(w);
			return menu_part();
		}
		Sleep(20);
	}
	return 1;
}
