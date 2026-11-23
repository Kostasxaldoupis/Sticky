#include <windows.h>
#include <richedit.h>
#include <commctrl.h>
#include <stdio.h>
#include <shlobj.h>

// Link these on MSVC (or add -luser32 -lgdi32 -lshell32 -lcomctl32 on MinGW)
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")

#define NOTE_WIDTH 350
#define NOTE_HEIGHT 350
#define EDIT_MARGIN 15
#define NOTE_COLOR RGB(255, 249, 170)
#define WM_APP_FOLD (WM_APP + 1)
#define IDT_AUTOSAVE 1
#define AUTOSAVE_MS 5000

// Folded height = title bar + a sliver of yellow
#define MINIMIZED_HEIGHT (GetSystemMetrics(SM_CYCAPTION) + \
                          GetSystemMetrics(SM_CYSIZEFRAME) * 2 + 20)

BOOL isMinimized = FALSE;
BOOL g_dirty = FALSE;
HWND hEdit = NULL;
HMODULE hRichEdit = NULL;
HBRUSH g_noteBrush = NULL;
HBRUSH g_editBrush = NULL;

// Last normal (unfolded) size, restored when unfolding.
static int g_restoreW = NOTE_WIDTH;
static int g_restoreH = NOTE_HEIGHT;

// Paths
static void GetSavePaths(char *rtfPath, size_t rtfLen,
                         char *iniPath, size_t iniLen)
{
    char appdata[MAX_PATH] = {0};
    if (FAILED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata)))
        GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH);

    char dir[MAX_PATH];
    snprintf(dir, sizeof(dir), "%s\\StickyNote", appdata);
    CreateDirectoryA(dir, NULL);

    snprintf(rtfPath, rtfLen, "%s\\note.rtf", dir);
    snprintf(iniPath, iniLen, "%s\\state.ini", dir);
}

// Atomic RTF save: write to .tmp, then rename over the real file
static DWORD CALLBACK RtfSaveCallback(DWORD_PTR dwCookie, LPBYTE pbBuff,
                                      LONG cb, LONG *pcb)
{
    FILE *fp = (FILE *)dwCookie;
    *pcb = (LONG)fwrite(pbBuff, 1, (size_t)cb, fp);
    return (*pcb == cb) ? 0 : 1; // non-zero aborts EM_STREAMOUT
}

static void SaveNoteText(HWND hEdit)
{
    char rtfPath[MAX_PATH], iniPath[MAX_PATH], tmpPath[MAX_PATH];
    GetSavePaths(rtfPath, sizeof(rtfPath), iniPath, sizeof(iniPath));
    snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", rtfPath);

    FILE *fp = fopen(tmpPath, "wb");
    if (!fp)
        return;

    EDITSTREAM es = {0};
    es.dwCookie = (DWORD_PTR)fp;
    es.pfnCallback = RtfSaveCallback;
    SendMessageW(hEdit, EM_STREAMOUT, SF_RTF, (LPARAM)&es);

    BOOL ok = (es.dwError == 0) && (ferror(fp) == 0);
    fclose(fp);

    if (ok)
        MoveFileExA(tmpPath, rtfPath, MOVEFILE_REPLACE_EXISTING);
    else
        DeleteFileA(tmpPath);

    g_dirty = FALSE;
}

// RTF load (clears existing content first)
static DWORD CALLBACK RtfLoadCallback(DWORD_PTR dwCookie, LPBYTE pbBuff,
                                      LONG cb, LONG *pcb)
{
    FILE *fp = (FILE *)dwCookie;
    *pcb = (LONG)fread(pbBuff, 1, (size_t)cb, fp);
    return ferror(fp) ? 1 : 0;
}

static void LoadNoteText(HWND hEdit)
{
    char rtfPath[MAX_PATH], iniPath[MAX_PATH];
    GetSavePaths(rtfPath, sizeof(rtfPath), iniPath, sizeof(iniPath));

    FILE *fp = fopen(rtfPath, "rb");
    if (!fp)
        return;

    // Replace, not append
    SendMessageW(hEdit, EM_SETSEL, 0, -1);
    SendMessageW(hEdit, EM_REPLACESEL, FALSE, (LPARAM)L"");

    EDITSTREAM es = {0};
    es.dwCookie = (DWORD_PTR)fp;
    es.pfnCallback = RtfLoadCallback;
    SendMessageW(hEdit, EM_STREAMIN, SF_RTF, (LPARAM)&es);
    fclose(fp);

    // Move caret to start
    SendMessageW(hEdit, EM_SETSEL, 0, 0);
}

// Window state save / load
static void SaveWindowState(HWND hwnd)
{
    char rtfPath[MAX_PATH], iniPath[MAX_PATH];
    GetSavePaths(rtfPath, sizeof(rtfPath), iniPath, sizeof(iniPath));

    RECT rc = {0};
    GetWindowRect(hwnd, &rc);

    // If folded, do not persist the tiny folded size as the normal size.
    int w = isMinimized ? g_restoreW : (rc.right - rc.left);
    int h = isMinimized ? g_restoreH : (rc.bottom - rc.top);

    char buf[32];
    snprintf(buf, sizeof(buf), "%d", (int)rc.left);
    WritePrivateProfileStringA("window", "x", buf, iniPath);
    snprintf(buf, sizeof(buf), "%d", (int)rc.top);
    WritePrivateProfileStringA("window", "y", buf, iniPath);
    snprintf(buf, sizeof(buf), "%d", w);
    WritePrivateProfileStringA("window", "w", buf, iniPath);
    snprintf(buf, sizeof(buf), "%d", h);
    WritePrivateProfileStringA("window", "h", buf, iniPath);
    snprintf(buf, sizeof(buf), "%d", isMinimized ? 1 : 0);
    WritePrivateProfileStringA("window", "folded", buf, iniPath);
}

static void LoadWindowStateInto(int *outX, int *outY, int *outW, int *outH, int *outFolded)
{
    char rtfPath[MAX_PATH], iniPath[MAX_PATH];
    GetSavePaths(rtfPath, sizeof(rtfPath), iniPath, sizeof(iniPath));

    int x = GetPrivateProfileIntA("window", "x", CW_USEDEFAULT, iniPath);
    int y = GetPrivateProfileIntA("window", "y", CW_USEDEFAULT, iniPath);
    int w = GetPrivateProfileIntA("window", "w", NOTE_WIDTH, iniPath);
    int h = GetPrivateProfileIntA("window", "h", NOTE_HEIGHT, iniPath);
    int folded = GetPrivateProfileIntA("window", "folded", 0, iniPath);

    RECT work;
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);
    if (x != CW_USEDEFAULT &&
        (x < work.left - 50 || x > work.right - 100 ||
         y < work.top - 50 || y > work.bottom - 100))
    {
        x = CW_USEDEFAULT;
        y = CW_USEDEFAULT;
    }

    *outX = x;
    *outY = y;
    *outW = w;
    *outH = h;
    *outFolded = folded;
}

// Bold toggle (with caret-format fallback for empty selections)
static void ToggleBold(HWND hEdit)
{
    CHARFORMAT2W cf = {0};
    cf.cbSize = sizeof(cf);
    SendMessageW(hEdit, EM_GETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);

    cf.dwEffects = (cf.dwEffects & CFE_BOLD) ? 0 : CFE_BOLD;
    cf.dwMask = CFM_BOLD;
    SendMessageW(hEdit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
}

// Resize + fold
static void ResizeEdit(HWND hwnd)
{
    if (!hEdit)
        return;
    RECT rc;
    GetClientRect(hwnd, &rc);
    SetWindowPos(hEdit, NULL,
                 EDIT_MARGIN, EDIT_MARGIN,
                 rc.right - 2 * EDIT_MARGIN,
                 rc.bottom - 2 * EDIT_MARGIN,
                 SWP_NOZORDER);
}

static void SetFolded(HWND hwnd, BOOL folded)
{
    if (folded == isMinimized)
        return;

    RECT rc;
    GetWindowRect(hwnd, &rc);

    if (folded)
    {
        // Remember the normal size before collapsing.
        g_restoreW = rc.right - rc.left;
        g_restoreH = rc.bottom - rc.top;
        isMinimized = TRUE;

        ShowWindow(hEdit, SW_HIDE);
        SetWindowPos(hwnd, NULL, rc.left, rc.top, NOTE_WIDTH, MINIMIZED_HEIGHT,
                     SWP_NOZORDER);
    }
    else
    {
        isMinimized = FALSE;
        SetWindowPos(hwnd, NULL, rc.left, rc.top, g_restoreW, g_restoreH,
                     SWP_NOZORDER);
        ShowWindow(hEdit, SW_SHOW);
        ResizeEdit(hwnd);
    }
}

static void ToggleMinimize(HWND hwnd) { SetFolded(hwnd, !isMinimized); }

// Edit subclass
static LRESULT CALLBACK EditSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam,
                                         LPARAM lParam, UINT_PTR uIdSubclass,
                                         DWORD_PTR dwRefData)
{
    (void)uIdSubclass;
    (void)dwRefData;

    switch (uMsg)
    {
    case WM_KEYDOWN:
        if (wParam == 'B' && (GetKeyState(VK_CONTROL) & 0x8000))
        {
            ToggleBold(hwnd);
            return 0;
        }
        break;

    case WM_LBUTTONDBLCLK:
        SendMessageW(GetParent(hwnd), WM_APP_FOLD, 0, 0);
        return 0;
    }

    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

// Main window proc
static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_LBUTTONDBLCLK:
        ToggleMinimize(hwnd);
        return 0;

    case WM_APP_FOLD:
        ToggleMinimize(hwnd);
        return 0;

    case WM_SYSCOMMAND:
        // Block maximize, but let the default handler process everything else.
        // This is required for SC_CLOSE, SC_MOVE/SC_SIZE, and the caption drag
        // loop; swallowing WM_SYSCOMMAND breaks close + move.
        if ((wParam & 0xFFF0) == SC_MAXIMIZE)
            return 0;
        return DefWindowProc(hwnd, uMsg, wParam, lParam);

    case WM_GETMINMAXINFO:
    {
        MINMAXINFO *mmi = (MINMAXINFO *)lParam;
        mmi->ptMinTrackSize.x = 200;
        mmi->ptMinTrackSize.y = MINIMIZED_HEIGHT;
        mmi->ptMaxTrackSize.x = 2000;
        mmi->ptMaxTrackSize.y = 2000;
        return 0;
    }

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED && !isMinimized)
            ResizeEdit(hwnd);
        return 0;

        // No WM_ERASEBKGND: wc.hbrBackground handles it, no per-paint brush churn

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
    {
        HDC hdc = (HDC)wParam;
        SetBkColor(hdc, NOTE_COLOR);
        SetTextColor(hdc, RGB(0, 0, 0));
        if (!g_editBrush)
            g_editBrush = CreateSolidBrush(NOTE_COLOR);
        return (LRESULT)g_editBrush;
    }

    case WM_CREATE:
    {
        hEdit = CreateWindowExW(0, L"RICHEDIT50W", L"",
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE |
                                    ES_AUTOVSCROLL | ES_NOHIDESEL,
                                EDIT_MARGIN, EDIT_MARGIN,
                                NOTE_WIDTH - 2 * EDIT_MARGIN,
                                NOTE_HEIGHT - 2 * EDIT_MARGIN,
                                hwnd, NULL, GetModuleHandle(NULL), NULL);

        if (!hEdit)
        {
            MessageBoxA(hwnd, "Could not create Rich Edit control.",
                        "Error", MB_ICONERROR);
            return -1;
        }

        SetWindowSubclass(hEdit, EditSubclassProc, 0, 0);
        SendMessageW(hEdit, EM_SETBKGNDCOLOR, 0, (LPARAM)NOTE_COLOR);

        // Default font for the empty first-run case
        {
            CHARFORMAT2W cf = {0};
            cf.cbSize = sizeof(cf);
            cf.dwMask = CFM_FACE | CFM_SIZE;
            cf.yHeight = 200; // 10pt
            wcscpy_s(cf.szFaceName, LF_FACESIZE, L"Segoe UI");
            SendMessageW(hEdit, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
        }

        // Mark dirty on every edit so the timer can skip no-op saves
        SendMessageW(hEdit, EM_SETEVENTMASK, 0,
                     SendMessageW(hEdit, EM_GETEVENTMASK, 0, 0) | ENM_CHANGE);

        LoadNoteText(hEdit);
        g_dirty = FALSE;

        SetTimer(hwnd, IDT_AUTOSAVE, AUTOSAVE_MS, NULL);
        return 0;
    }

    case WM_TIMER:
        if (wParam == IDT_AUTOSAVE && g_dirty)
            SaveNoteText(hEdit);
        return 0;

    case WM_COMMAND:
        if (HIWORD(wParam) == EN_CHANGE && (HWND)lParam == hEdit)
            g_dirty = TRUE;
        return 0;

    case WM_QUERYENDSESSION:
        if (g_dirty)
            SaveNoteText(hEdit);
        SaveWindowState(hwnd);
        return TRUE;

    case WM_CLOSE:
        if (g_dirty)
            SaveNoteText(hEdit);
        SaveWindowState(hwnd);
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, IDT_AUTOSAVE);
        if (g_editBrush)
        {
            DeleteObject(g_editBrush);
            g_editBrush = NULL;
        }
        if (g_noteBrush)
        {
            DeleteObject(g_noteBrush);
            g_noteBrush = NULL;
        }
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
}

// WinMain
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    (void)hPrevInstance;
    (void)lpCmdLine;

    const char CLASS_NAME[] = "StickyNoteWindow";

    hRichEdit = LoadLibraryA("Msftedit.dll");
    if (!hRichEdit)
    {
        MessageBoxA(NULL, "Could not load Msftedit.dll.", "Error", MB_ICONERROR);
        return 0;
    }

    g_noteBrush = CreateSolidBrush(NOTE_COLOR);

    WNDCLASSA wc = {0};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = g_noteBrush;

    if (!RegisterClassA(&wc))
    {
        FreeLibrary(hRichEdit);
        return 0;
    }

    int x, y, w, h, folded;
    LoadWindowStateInto(&x, &y, &w, &h, &folded);
    g_restoreW = w;
    g_restoreH = h;

    HWND hwnd = CreateWindowExA(
        0, CLASS_NAME, "My Sticky Note",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX |
            WS_THICKFRAME | WS_CLIPCHILDREN,
        x, y, w, h,
        NULL, NULL, hInstance, NULL);

    if (!hwnd)
    {
        FreeLibrary(hRichEdit);
        return 0;
    }

    if (folded)
        SetFolded(hwnd, TRUE);

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);

    MSG msg = {0};
    while (GetMessage(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    FreeLibrary(hRichEdit);
    return 0;
}

// gcc main.c -o StickyNotes.exe -mwindows -luser32 -lgdi32 -lshell32 -lcomctl32