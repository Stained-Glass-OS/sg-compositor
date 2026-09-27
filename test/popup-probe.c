/*
 * popup-probe -- a Windows program with a combo box, for popup-gate.sh.
 * Its list is an override-redirect X window (Setup runs without a Wine
 * desktop). Prints "COMBO x y w h", then "LIST x y w h" while the list is
 * open, "SEL n" when a choice is made; exits after 60 s or on "SEL".
 *
 * Copyright (C) 2026 David Hamner and the Stained Glass OS contributors
 * SPDX-License-Identifier: MIT
 */
#include <windows.h>
#include <stdio.h>

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
			return 0;
		}
		Sleep(20);
	}
	return 1;
}
