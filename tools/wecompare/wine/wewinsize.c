/*
 * wewinsize.exe <client width> <client height> - resizes live WE's -playInWindow window from inside Wine the way a
 * user drag does on Windows: SetWindowPos on the WPEOverlappedWallpaper window (WE 2.8.42 sub_14011E9D0, style
 * 0xCF0000), outer size from AdjustWindowRectEx so the client area gets the requested size. WE's WM_SIZE
 * (sub_14011E8E0) then hands the client rect to the wallpaper, which moves its render window and resizes the
 * swap chain and scene (sub_140115B90 -> sub_14017F1B0). Prints the client rect it ends up with.
 */
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

int main (int argc, char** argv) {
    if (argc < 3) {
	fprintf (stderr, "usage: wewinsize.exe <client width> <client height>\n");
	return 2;
    }

    HWND window = FindWindowW (L"WPEOverlappedWallpaper", NULL);
    if (window == NULL) {
	fprintf (stderr, "no WPEOverlappedWallpaper window\n");
	return 1;
    }

    RECT rect = { 0, 0, atoi (argv[1]), atoi (argv[2]) };
    AdjustWindowRectEx (&rect, (DWORD) GetWindowLongW (window, GWL_STYLE), FALSE, (DWORD) GetWindowLongW (window, GWL_EXSTYLE));
    SetWindowPos (window, NULL, 0, 0, rect.right - rect.left, rect.bottom - rect.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    RECT client;
    POINT origin = { 0, 0 };
    GetClientRect (window, &client);
    ClientToScreen (window, &origin);
    printf ("%ld %ld %ld %ld\n", origin.x, origin.y, client.right, client.bottom);
    return 0;
}
