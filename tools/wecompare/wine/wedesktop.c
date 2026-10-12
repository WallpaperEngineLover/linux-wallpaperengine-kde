/*
 * Stand-in desktop shell windows so live WE under Wine runs in desktop mode. WE 2.8.42 (sub_14003C840/sub_14003C760)
 * sends Progman 0x52C, then parents its window to the WorkerW below the one holding SHELLDLL_DefView.
 * Run before wallpaper64.exe with the same wine prefix; we_full.sh builds it.
 */
#include <windows.h>

static HWND g_progman, g_defView, g_iconHost, g_wallpaperHost;

static LRESULT CALLBACK windowProc (HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_ERASEBKGND) {
	RECT rect;
	GetClientRect (hwnd, &rect);
	FillRect ((HDC) wParam, &rect, (HBRUSH) GetStockObject (BLACK_BRUSH));
	return 1;
    }

    if (hwnd == g_progman && msg == 0x52C && g_wallpaperHost == NULL) {
	const int x = GetSystemMetrics (SM_XVIRTUALSCREEN), y = GetSystemMetrics (SM_YVIRTUALSCREEN);
	const int w = GetSystemMetrics (SM_CXVIRTUALSCREEN), h = GetSystemMetrics (SM_CYVIRTUALSCREEN);
	HINSTANCE instance = GetModuleHandleW (NULL);

	g_iconHost = CreateWindowExW (
	    WS_EX_TOOLWINDOW, L"WorkerW", L"", WS_POPUP | WS_CLIPCHILDREN, x, y, 1, 1, NULL, NULL, instance, NULL
	);
	SetParent (g_defView, g_iconHost);
	g_wallpaperHost = CreateWindowExW (
	    WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"WorkerW", L"", WS_POPUP | WS_CLIPCHILDREN | WS_VISIBLE, x, y, w, h,
	    NULL, NULL, instance, NULL
	);
	SetWindowPos (g_wallpaperHost, g_iconHost, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	return 0;
    }

    if (msg == WM_DESTROY && hwnd == g_progman) {
	PostQuitMessage (0);
    }

    return DefWindowProcW (hwnd, msg, wParam, lParam);
}

static void registerClass (const wchar_t* name) {
    WNDCLASSW cls = { 0 };
    cls.lpfnWndProc = windowProc;
    cls.hInstance = GetModuleHandleW (NULL);
    cls.lpszClassName = name;
    cls.hCursor = LoadCursorW (NULL, (LPCWSTR) IDC_ARROW);
    RegisterClassW (&cls);
}

int WINAPI wWinMain (HINSTANCE instance, HINSTANCE previous, PWSTR commandLine, int show) {
    registerClass (L"Progman");
    registerClass (L"WorkerW");
    registerClass (L"SHELLDLL_DefView");
    registerClass (L"SysListView32");

    const int w = GetSystemMetrics (SM_CXVIRTUALSCREEN), h = GetSystemMetrics (SM_CYVIRTUALSCREEN);

    g_progman = CreateWindowExW (
	WS_EX_TOOLWINDOW, L"Progman", L"Program Manager", WS_POPUP | WS_CLIPCHILDREN, 0, 0, w, h, NULL, NULL, instance,
	NULL
    );
    g_defView = CreateWindowExW (
	0, L"SHELLDLL_DefView", L"", WS_CHILD | WS_VISIBLE, 0, 0, w, h, g_progman, NULL, instance, NULL
    );
    CreateWindowExW (
	0, L"SysListView32", L"FolderView", WS_CHILD | WS_VISIBLE, 0, 0, w, h, g_defView, NULL, instance, NULL
    );

    MSG msg;

    while (GetMessageW (&msg, NULL, 0, 0) > 0) {
	TranslateMessage (&msg);
	DispatchMessageW (&msg);
    }

    return 0;
}
