/**
 * @file win32drv.c
 *
 * Based on win32drv of lv_drivers v8.3.0 (MIT license), see win32drv.h and README.md in this
 * folder for what changed.
 */

#include "win32drv.h"

#include <process.h>
#include <stdint.h>
#include <stdlib.h>
#include <windowsx.h>

#define WINDOW_EX_STYLE WS_EX_CLIENTEDGE
#define WINDOW_STYLE (WS_OVERLAPPEDWINDOW & ~(WS_SIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME))
#define KEY_QUEUE_SIZE 32

typedef struct {
    HANDLE ready;
    HINSTANCE instance;
    int show_mode;
} window_thread_parameter;

/* a 16 bit DIB needs its channel masks after the header */
typedef struct {
    BITMAPINFOHEADER header;
    DWORD masks[3];
} bitmap_info_565;

bool lv_win32_quit_signal = false;
lv_indev_t *lv_win32_pointer_device_object = NULL;

static CRITICAL_SECTION g_lock;
static HWND g_window = NULL;
static lv_disp_t *g_display = NULL;

/* the frame, in the orientation of the display now; its memory holds width * height pixels in
 * every orientation */
static lv_color_t *g_frame = NULL;
static lv_coord_t g_width = 0;
static lv_coord_t g_height = 0;
static int g_zoom = 1;

static volatile LONG g_mouse_pressed = 0;
static volatile LONG g_mouse_x = 0;
static volatile LONG g_mouse_y = 0;

static bool g_override_active = false;
static lv_coord_t g_override_x = 0;
static lv_coord_t g_override_y = 0;
static bool g_override_pressed = false;

static int g_keys[KEY_QUEUE_SIZE];
static int g_key_head = 0;
static int g_key_count = 0;

/* the window gets the size of the frame times the zoom; the border comes on top */
static void fit_window(HWND window)
{
    RECT rect = {0, 0, g_width * g_zoom, g_height * g_zoom};
    AdjustWindowRectEx(&rect, WINDOW_STYLE, FALSE, WINDOW_EX_STYLE);
    SetWindowPos(window, NULL, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
}

static void push_key(int key)
{
    EnterCriticalSection(&g_lock);
    if (g_key_count < KEY_QUEUE_SIZE) {
        g_keys[(g_key_head + g_key_count) % KEY_QUEUE_SIZE] = key;
        g_key_count++;
    }
    LeaveCriticalSection(&g_lock);
}

static void paint(HWND window)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(window, &ps);

    EnterCriticalSection(&g_lock);
    bitmap_info_565 info = {0};
    info.header.biSize = sizeof(BITMAPINFOHEADER);
    info.header.biWidth = g_width;
    info.header.biHeight = -g_height; /* top-down */
    info.header.biPlanes = 1;
    info.header.biBitCount = 16;
    info.header.biCompression = BI_BITFIELDS;
    info.masks[0] = 0xF800;
    info.masks[1] = 0x07E0;
    info.masks[2] = 0x001F;
    /* pixels are repeated, not smoothed: what the panel shows, magnified */
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, 0, 0, g_width * g_zoom, g_height * g_zoom, 0, 0, g_width, g_height, g_frame,
                  (const BITMAPINFO *)&info, DIB_RGB_COLORS, SRCCOPY);
    LeaveCriticalSection(&g_lock);

    EndPaint(window, &ps);
}

static LRESULT CALLBACK window_message_callback(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
        InterlockedExchange(&g_mouse_x, GET_X_LPARAM(lparam) / g_zoom);
        InterlockedExchange(&g_mouse_y, GET_Y_LPARAM(lparam) / g_zoom);
        if (message == WM_LBUTTONDOWN) {
            /* a drag that leaves the window still ends with a release */
            SetCapture(window);
            InterlockedExchange(&g_mouse_pressed, 1);
        } else if (message == WM_LBUTTONUP) {
            ReleaseCapture();
            InterlockedExchange(&g_mouse_pressed, 0);
        }
        return 0;
    case WM_KEYDOWN:
        if ((lparam & (1 << 30)) == 0) { /* not the auto-repeat */
            push_key((int)wparam | ((GetKeyState(VK_SHIFT) & 0x8000) != 0 ? 0x100 : 0));
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

static unsigned int __stdcall window_thread_entrypoint(void *raw_parameter)
{
    window_thread_parameter *parameter = (window_thread_parameter *)raw_parameter;

    WNDCLASSEXW window_class = {0};
    window_class.cbSize = sizeof(WNDCLASSEXW);
    window_class.lpfnWndProc = window_message_callback;
    window_class.hInstance = parameter->instance;
    window_class.hCursor = LoadCursorW(NULL, IDC_ARROW);
    window_class.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    window_class.lpszClassName = L"hassctrl_sim";

    HWND window = NULL;
    if (RegisterClassExW(&window_class)) {
        window = CreateWindowExW(WINDOW_EX_STYLE, window_class.lpszClassName, L"hassctrl sim", WINDOW_STYLE,
                                 CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, NULL, NULL, parameter->instance, NULL);
    }
    if (window != NULL) {
        fit_window(window);
        ShowWindow(window, parameter->show_mode);
        UpdateWindow(window);
    }
    g_window = window;
    SetEvent(parameter->ready);
    if (window == NULL) {
        return 0;
    }

    MSG message;
    while (GetMessageW(&message, NULL, 0, 0)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    lv_win32_quit_signal = true;
    return 0;
}

static void display_flush_callback(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *color_p)
{
    (void)area;
    (void)color_p; /* direct mode: LVGL drew into g_frame itself */
    if (lv_disp_flush_is_last(driver) && g_window != NULL) {
        InvalidateRect(g_window, NULL, FALSE);
    }
    lv_disp_flush_ready(driver);
}

/* lv_disp_set_rotation() ends here: the window takes the new orientation, like the panel does in
 * the firmware (the drawing area turns in hardware) */
static void display_update_callback(lv_disp_drv_t *driver)
{
    (void)driver;
    EnterCriticalSection(&g_lock);
    g_width = lv_disp_get_hor_res(g_display);
    g_height = lv_disp_get_ver_res(g_display);
    LeaveCriticalSection(&g_lock);
    if (g_window != NULL) {
        fit_window(g_window);
    }
}

static void pointer_read_callback(lv_indev_drv_t *driver, lv_indev_data_t *data)
{
    (void)driver;
    EnterCriticalSection(&g_lock);
    lv_coord_t x = (lv_coord_t)g_mouse_x;
    lv_coord_t y = (lv_coord_t)g_mouse_y;
    bool pressed = g_mouse_pressed != 0;
    if (g_override_active) {
        x = g_override_x;
        y = g_override_y;
        pressed = g_override_pressed;
    }
    const lv_coord_t width = g_width;
    const lv_coord_t height = g_height;
    LeaveCriticalSection(&g_lock);

    /* x and y are in the orientation of the window. LVGL turns what an input device reports as
     * if it came from the panel in its base orientation (lv_indev.c, indev_pointer_proc), so the
     * point goes back through the same turn: what a touch controller would report. */
    x = x < 0 ? 0 : (x > width - 1 ? width - 1 : x);
    y = y < 0 ? 0 : (y > height - 1 ? height - 1 : y);
    const lv_coord_t hor = g_display->driver->hor_res;
    const lv_coord_t ver = g_display->driver->ver_res;
    lv_coord_t raw_x = x;
    lv_coord_t raw_y = y;
    switch (g_display->driver->rotated) {
    case LV_DISP_ROT_90:
        raw_x = y;
        raw_y = ver - x - 1;
        break;
    case LV_DISP_ROT_180:
        raw_x = hor - x - 1;
        raw_y = ver - y - 1;
        break;
    case LV_DISP_ROT_270:
        raw_x = hor - y - 1;
        raw_y = x;
        break;
    default:
        break;
    }
    data->state = pressed ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
    data->point.x = raw_x;
    data->point.y = raw_y;
}

bool lv_win32_init(HINSTANCE instance_handle, int show_window_mode, lv_coord_t hor_res, lv_coord_t ver_res,
                   int zoom)
{
    InitializeCriticalSection(&g_lock);
    g_zoom = zoom < 1 ? 1 : zoom;
    g_width = hor_res;
    g_height = ver_res;
    g_frame = (lv_color_t *)calloc((size_t)hor_res * (size_t)ver_res, sizeof(lv_color_t));
    if (g_frame == NULL) {
        return false;
    }

    window_thread_parameter parameter = {CreateEventExW(NULL, NULL, 0, EVENT_ALL_ACCESS), instance_handle,
                                         show_window_mode};
    _beginthreadex(NULL, 0, window_thread_entrypoint, &parameter, 0, NULL);
    WaitForSingleObjectEx(parameter.ready, INFINITE, FALSE);
    CloseHandle(parameter.ready);
    if (g_window == NULL) {
        return false;
    }

    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, g_frame, NULL, (uint32_t)hor_res * (uint32_t)ver_res);

    static lv_disp_drv_t display_driver;
    lv_disp_drv_init(&display_driver);
    display_driver.hor_res = hor_res;
    display_driver.ver_res = ver_res;
    display_driver.flush_cb = display_flush_callback;
    display_driver.drv_update_cb = display_update_callback;
    display_driver.draw_buf = &draw_buf;
    display_driver.direct_mode = 1;
    g_display = lv_disp_drv_register(&display_driver);

    static lv_indev_drv_t pointer_driver;
    lv_indev_drv_init(&pointer_driver);
    pointer_driver.type = LV_INDEV_TYPE_POINTER;
    pointer_driver.read_cb = pointer_read_callback;
    lv_win32_pointer_device_object = lv_indev_drv_register(&pointer_driver);
    return true;
}

void lv_win32_set_title(const char *title)
{
    if (g_window == NULL) {
        return;
    }
    wchar_t wide[256];
    if (MultiByteToWideChar(CP_UTF8, 0, title, -1, wide, 256) > 0) {
        SetWindowTextW(g_window, wide);
    }
}

const lv_color_t *lv_win32_framebuffer(lv_coord_t *width, lv_coord_t *height)
{
    *width = g_width;
    *height = g_height;
    return g_frame;
}

void lv_win32_pointer_override(bool active, lv_coord_t x, lv_coord_t y, bool pressed)
{
    EnterCriticalSection(&g_lock);
    g_override_active = active;
    g_override_x = x;
    g_override_y = y;
    g_override_pressed = pressed;
    LeaveCriticalSection(&g_lock);
}

int lv_win32_pop_key(void)
{
    int key = 0;
    EnterCriticalSection(&g_lock);
    if (g_key_count > 0) {
        key = g_keys[g_key_head];
        g_key_head = (g_key_head + 1) % KEY_QUEUE_SIZE;
        g_key_count--;
    }
    LeaveCriticalSection(&g_lock);
    return key;
}
