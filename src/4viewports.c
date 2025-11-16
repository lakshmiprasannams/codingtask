#include <Windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_VIEWPORTS 4
#define TIMER_ID 1

#define FILE_HEADER_TAG "FILE"
#define CONTENT_HEADER_TAG "IMAG"
#define CONTENT_TRAILER_TAG "TRAI"


// --- DAT file structures, packed for x64 MSVC ---
#pragma pack(push,1)
typedef struct {
    char Signature[5];       // FILE\0
    uint8_t  preamble0;      // = 0x05
    uint16_t NumImages;
    uint16_t CycleTimeMs;
} DatFileHeader;
#pragma pack(pop)

#pragma pack(push,1)
typedef struct {
    char Signature[5];   // 0..4
    uint8_t reserved[3]; // padding to align next field to 8
    size_t PayloadSize;  // 8 bytes at offset 8
} DatFileContentHeader;
#pragma pack(pop)

#pragma pack(push,1)
typedef struct {
    char Signature[5];       // TRAI\0
    uint8_t reserved[3]; // padding to align next field to 8
    size_t PrevContentSize;
} DatFileContentTrailer;
#pragma pack(pop)

// --- Bitmap storage ---
typedef struct {
    HBITMAP hBitmap;       // Loaded BMP handle
    uint16_t delay;        // milliseconds
} BitmapEntry;

typedef struct {
    int numBitmaps;
    BitmapEntry* bitmaps;
} DatFile;

// --- Viewport ---
typedef struct {
    HWND hwnd;
    DatFile* dat;
    int currentFrame;
    DWORD lastUpdateTime;
} Viewport;


Viewport g_Viewports[MAX_VIEWPORTS];

// --- Load BMP from memory ---
HBITMAP LoadBMPFromMemory(char* data, uint64_t size)
{
    if (!data || size == 0) return NULL;

    char tmpPath[MAX_PATH];
    GetTempPathA(MAX_PATH, tmpPath);
    strcat_s(tmpPath, MAX_PATH, "tmp.bmp");

    FILE* f = NULL;
    if (fopen_s(&f, tmpPath, "wb") != 0 || !f) return NULL;
    fwrite(data, 1, size, f);
    fclose(f);

    return (HBITMAP)LoadImageA(NULL, tmpPath, IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE);
}

// --- Read binary DAT file ---
// --- Read binary DAT file with debug logging ---
DatFile* ReadDatFile(const char* path)
{
    if (!path) return NULL;

    FILE* fp = NULL;
    if (fopen_s(&fp, path, "rb") != 0 || !fp) {
        char msg[256]; sprintf_s(msg, sizeof(msg), "Failed to open file: %s", path);
        MessageBoxA(NULL, msg, "Error", MB_OK | MB_ICONERROR);
        return NULL;
    }

    DatFileHeader header;
    if (fread(&header, sizeof(header), 1, fp) != 1) {
        MessageBoxA(NULL, "Failed to read DAT file header", "Error", MB_OK | MB_ICONERROR);
        fclose(fp);
        return NULL;
    }

    char debugMsg[256];
    sprintf_s(debugMsg, sizeof(debugMsg), "DAT Header: Signature=%.4s NumImages=%d CycleTime=%d",
        header.Signature, header.NumImages, header.CycleTimeMs);
    OutputDebugStringA(debugMsg);

    if (memcmp(header.Signature, FILE_HEADER_TAG, 4) != 0) {
        MessageBoxA(NULL, "DAT file signature invalid", "Error", MB_OK | MB_ICONERROR);
        fclose(fp);
        return NULL;
    }

    DatFile* dat = (DatFile*)calloc(1, sizeof(DatFile));
    dat->numBitmaps = header.NumImages;
    dat->bitmaps = (BitmapEntry*)calloc(dat->numBitmaps, sizeof(BitmapEntry));

    for (int i = 0; i < dat->numBitmaps; i++)
    {
        // 1. Read Content Header
        DatFileContentHeader ch;
        if (fread(&ch, sizeof(ch), 1, fp) != 1) goto fail;

        sprintf_s(debugMsg, sizeof(debugMsg),
            "Image %d Header: Signature=%.4s PayloadSize=%llu",
            i, ch.Signature, (unsigned long long)ch.PayloadSize);
        OutputDebugStringA(debugMsg);

        if (memcmp(ch.Signature, CONTENT_HEADER_TAG, 4) != 0) goto fail;

        // 2. Read payload
        char* bmpData = (char*)malloc((size_t)ch.PayloadSize);
        if (!bmpData) goto fail;

        if (fread(bmpData, 1, (size_t)ch.PayloadSize, fp) != (size_t)ch.PayloadSize) {
            free(bmpData);
            goto fail;
        }

        // 3. Dump the BMP to disk for debugging
        {
            char filename[128];
            sprintf_s(filename, sizeof(filename), "frame_%d.bmp", i);

            FILE* out = NULL;
            if (fopen_s(&out, filename, "wb") == 0 && out) {
                fwrite(bmpData, 1, (size_t)ch.PayloadSize, out);
                fclose(out);
            }
        }

        // 4. Convert to HBITMAP
        HBITMAP hBmp = LoadBMPFromMemory(bmpData, (size_t)ch.PayloadSize);
        free(bmpData);
        if (!hBmp) goto fail;

        dat->bitmaps[i].hBitmap = hBmp;
        dat->bitmaps[i].delay = header.CycleTimeMs;

        // 5. Read trailer
        DatFileContentTrailer ct;
        if (fread(&ct, sizeof(ct), 1, fp) != 1) goto fail;

        sprintf_s(debugMsg, sizeof(debugMsg),
            "Image %d Trailer: Signature=%.4s PrevSize=%llu",
            i, ct.Signature, (unsigned long long)ct.PrevContentSize);
        OutputDebugStringA(debugMsg);

        if (memcmp(ct.Signature, CONTENT_TRAILER_TAG, 4) != 0) goto fail;
        if (ct.PrevContentSize != ch.PayloadSize) goto fail;
    }

    fclose(fp);
    return dat;

fail:
    for (int j = 0; j < dat->numBitmaps; j++)
        if (dat->bitmaps[j].hBitmap) DeleteObject(dat->bitmaps[j].hBitmap);
    free(dat->bitmaps);
    free(dat);
    fclose(fp);
    MessageBoxA(NULL, "Failed to parse DAT file correctly", "Error", MB_OK | MB_ICONERROR);
    return NULL;
}



// --- Viewport window procedure ---
LRESULT CALLBACK ViewportWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        // Get viewport size
        RECT client;
        GetClientRect(hwnd, &client);
        int viewportWidth = client.right - client.left;
        int viewportHeight = client.bottom - client.top;

        // Create off-screen DC (back buffer) and bitmap
        HDC backDC = CreateCompatibleDC(hdc);
        HBITMAP backBmp = CreateCompatibleBitmap(hdc, viewportWidth, viewportHeight);
        HGDIOBJ oldBack = SelectObject(backDC, backBmp);

        // Fill background (optional)
        FillRect(backDC, &client, (HBRUSH)(COLOR_WINDOW + 1));

        // Draw all bitmaps to back buffer
        for (int i = 0; i < MAX_VIEWPORTS; i++)
        {
            if (g_Viewports[i].hwnd != hwnd || !g_Viewports[i].dat) continue;

            int frame = g_Viewports[i].currentFrame;
            DatFile* dat = g_Viewports[i].dat;
            if (frame < 0 || frame >= dat->numBitmaps) continue;

            HBITMAP hBmp = dat->bitmaps[frame].hBitmap;
            if (!hBmp) continue;

            HDC memDC = CreateCompatibleDC(backDC);
            HGDIOBJ oldMem = SelectObject(memDC, hBmp);

            BITMAP bm;
            GetObject(hBmp, sizeof(BITMAP), &bm);

            // Stretch bitmap to fit viewport
            StretchBlt(backDC, 0, 0, viewportWidth, viewportHeight,
                memDC, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);

            SelectObject(memDC, oldMem);
            DeleteDC(memDC);
        }

        // Blit the back buffer to the screen in one operation
        BitBlt(hdc, 0, 0, viewportWidth, viewportHeight, backDC, 0, 0, SRCCOPY);

        // Cleanup
        SelectObject(backDC, oldBack);
        DeleteObject(backBmp);
        DeleteDC(backDC);

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_KEYDOWN:
        if (wParam == VK_F5)
        {
            // Forward F5 to main window
            HWND hwndParent = GetParent(hwnd);
            if (hwndParent)
                PostMessage(hwndParent, WM_KEYDOWN, wParam, lParam);
        }
        break;

    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// --- Register viewport class ---
ATOM RegisterViewportClass(HINSTANCE hInst)
{
    WNDCLASSEXA wc = { 0 };
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ViewportWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = "ViewportChild";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    return RegisterClassExA(&wc);
}

// --- Create 4 viewports ---
void CreateViewports(HWND hwndParent, HINSTANCE hInst)
{
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    char* lastSlash = strrchr(exePath, '\\'); if (lastSlash) *lastSlash = '\0';

    for (int i = 0; i < MAX_VIEWPORTS; i++)
    {
        g_Viewports[i].hwnd = CreateWindowExA(0, "ViewportChild", NULL,
            WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP,
            0, 0, 100, 100, hwndParent, NULL, hInst, NULL);

        char datPath[MAX_PATH];
        sprintf_s(datPath, sizeof(datPath), "%s\\v%d.dat", exePath, i);
        g_Viewports[i].dat = ReadDatFile(datPath);

        if (!g_Viewports[i].dat)
        {
            char msg[256];
            sprintf_s(msg, sizeof(msg), "Viewport %d failed to load DAT: %s", i, datPath);
            MessageBoxA(NULL, msg, "Error", MB_OK | MB_ICONERROR);
        }

        g_Viewports[i].currentFrame = 0;
        g_Viewports[i].lastUpdateTime = GetTickCount();
    }
}

// --- Main window procedure ---
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_SIZE:
    {
        int w = LOWORD(lParam);
        int h = HIWORD(lParam);
        int halfW = w / 2;
        int halfH = h / 2;

        MoveWindow(g_Viewports[0].hwnd, 0, 0, halfW, halfH, TRUE);
        MoveWindow(g_Viewports[1].hwnd, halfW, 0, halfW, halfH, TRUE);
        MoveWindow(g_Viewports[2].hwnd, 0, halfH, halfW, halfH, TRUE);
        MoveWindow(g_Viewports[3].hwnd, halfW, halfH, halfW, halfH, TRUE);
        break;
    }

    case WM_TIMER:
    {
        DWORD now = GetTickCount();
        for (int i = 0; i < MAX_VIEWPORTS; i++)
        {
            if (!g_Viewports[i].dat) continue;

            BitmapEntry* bmp = &g_Viewports[i].dat->bitmaps[g_Viewports[i].currentFrame];
            if (now - g_Viewports[i].lastUpdateTime >= bmp->delay)
            {
                g_Viewports[i].currentFrame++;
                if (g_Viewports[i].currentFrame >= g_Viewports[i].dat->numBitmaps)
                    g_Viewports[i].currentFrame = 0;

                g_Viewports[i].lastUpdateTime = now;
                InvalidateRect(g_Viewports[i].hwnd, NULL, TRUE);
            }
        }
        break;
    }

    case WM_KEYDOWN:
    {
        if (wParam == VK_F5)
        {
            // Reset all viewports to first frame
            for (int i = 0; i < MAX_VIEWPORTS; i++)
            {
                if (g_Viewports[i].dat)
                {
                    g_Viewports[i].currentFrame = 0;
                    g_Viewports[i].lastUpdateTime = GetTickCount();
                    InvalidateRect(g_Viewports[i].hwnd, NULL, TRUE);
                }
            }
        }
        break;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
    return 0;
}


// --- WinMain ---
int APIENTRY WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmdLine, int nCmdShow)
{
    WNDCLASSEXA wc = { 0 };
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = "MainWindowClass";
    RegisterClassExA(&wc);

    HWND hwndMain = CreateWindowExA(0, "MainWindowClass", "4 Viewports Demo",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
        NULL, NULL, hInst, NULL);

    if (!hwndMain)
    {
        MessageBoxA(NULL, "Failed to create main window", "Error", MB_OK);
        return 0;
    }

    RegisterViewportClass(hInst);
    CreateViewports(hwndMain, hInst);

    ShowWindow(hwndMain, nCmdShow);
    UpdateWindow(hwndMain);

    SetTimer(hwndMain, TIMER_ID, 16, NULL);

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    // Cleanup
    for (int i = 0; i < MAX_VIEWPORTS; i++)
    {
        if (g_Viewports[i].dat)
        {
            for (int j = 0; j < g_Viewports[i].dat->numBitmaps; j++)
                if (g_Viewports[i].dat->bitmaps[j].hBitmap)
                    DeleteObject(g_Viewports[i].dat->bitmaps[j].hBitmap);

            free(g_Viewports[i].dat->bitmaps);
            free(g_Viewports[i].dat);
        }
    }

    return (int)msg.wParam;
}
