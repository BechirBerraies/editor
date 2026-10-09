// win32_editor.c - the Windows platform layer: window, back buffer, input, files, dialogs, DPI.
// Unity build: the editor core (with its keybinds) and its self-test are compiled into this
// translation unit, before windows.h, so the core can't depend on it.
#include "editor.c"
#include "editor_tests.c"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>

typedef BOOL (WINAPI *SetProcessDpiAwarenessContextFn)(HANDLE context);
typedef UINT (WINAPI *GetDpiForWindowFn)(HWND window);

typedef struct Win32Writer {
    HANDLE handle;
    char tmp_path[EDITOR_PATH_MAX + 8];
    char path[EDITOR_PATH_MAX];
} Win32Writer;

static HWND g_window;
static Memory g_memory;
static Platform g_platform;
static Bitmap g_bitmap;
static BITMAPINFO g_bitmap_info;
static Win32Writer g_writer;
static GetDpiForWindowFn g_get_dpi_for_window;
static int g_wheel_remainder;
static b32 g_editor_ready;
static b32 g_in_editor;     // the editor may be inside a modal dialog
static b32 g_resize_pending;

//
// Platform callbacks
//

static b32 win32_commit(void *base, u64 size)
{
    return VirtualAlloc(base, (SIZE_T)size, MEM_COMMIT, PAGE_READWRITE) != 0;
}

static s64 win32_file_size(const char *path)
{
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &data)) {
        DWORD error = GetLastError();
        return (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) ? -1 : -2;
    }
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return -2;
    return (s64)(((u64)data.nFileSizeHigh << 32) | data.nFileSizeLow);
}

static b32 win32_read_file(const char *path, void *dest, u64 size)
{
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, 0);
    if (file == INVALID_HANDLE_VALUE) return 0;

    u8 *at = dest;
    u64 left = size;
    b32 ok = 1;
    while (ok && left) {
        DWORD chunk = (DWORD)MIN(left, MEGABYTES(64));
        DWORD got = 0;
        ok = ReadFile(file, at, chunk, &got, 0) && got == chunk;
        at += got;
        left -= got;
    }
    CloseHandle(file);
    return ok;
}

static void *win32_begin_write(const char *path)
{
    if (g_writer.handle) return 0;
    if (str_len(path) + 5 > sizeof(g_writer.tmp_path)) return 0;
    str_copy(g_writer.path, sizeof(g_writer.path), path);
    str_copy(g_writer.tmp_path, sizeof(g_writer.tmp_path), path);
    str_append(g_writer.tmp_path, sizeof(g_writer.tmp_path), ".tmp");

    HANDLE file = CreateFileA(g_writer.tmp_path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (file == INVALID_HANDLE_VALUE) return 0;
    g_writer.handle = file;
    return &g_writer;
}

static b32 win32_write(void *file, const void *data, u64 size)
{
    Win32Writer *writer = file;
    const u8 *at = data;
    while (size) {
        DWORD chunk = (DWORD)MIN(size, MEGABYTES(64));
        DWORD written = 0;
        if (!WriteFile(writer->handle, at, chunk, &written, 0) || written != chunk) return 0;
        at += written;
        size -= written;
    }
    return 1;
}

static b32 win32_end_write(void *file, b32 keep)
{
    Win32Writer *writer = file;
    b32 ok = keep && FlushFileBuffers(writer->handle);
    CloseHandle(writer->handle);
    writer->handle = 0;
    if (ok) ok = MoveFileExA(writer->tmp_path, writer->path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) DeleteFileA(writer->tmp_path);
    return ok;
}

static b32 win32_list_directory(const char *dir, DirEntryFn on_entry, void *ctx)
{
    char pattern[EDITOR_PATH_MAX + 4];
    if (str_len(dir) + 3 > sizeof(pattern)) return 0;
    str_copy(pattern, sizeof(pattern), dir);
    str_append(pattern, sizeof(pattern), "\\*");

    WIN32_FIND_DATAA data;
    HANDLE find = FindFirstFileExA(pattern, FindExInfoBasic, &data, FindExSearchNameMatch, 0, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) return 0;
    do {
        const char *name = data.cFileName;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        on_entry(ctx, name, (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    } while (FindNextFileA(find, &data));
    FindClose(find);
    return 1;
}

static b32 win32_file_dialog(char *path, u64 cap, b32 save)
{
    char buffer[EDITOR_PATH_MAX];
    str_copy(buffer, sizeof(buffer), save ? path : "");

    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_window;
    ofn.lpstrFilter = "All Files (*.*)\0*.*\0Text Files (*.txt)\0*.txt\0";
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = sizeof(buffer);
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    b32 ok;
    if (save) {
        ofn.Flags |= OFN_OVERWRITEPROMPT;
        ok = GetSaveFileNameA(&ofn);
    } else {
        ofn.Flags |= OFN_FILEMUSTEXIST;
        ok = GetOpenFileNameA(&ofn);
    }
    if (ok) str_copy(path, cap, buffer);
    return ok;
}

static b32 win32_open_dialog(char *path, u64 cap)
{
    return win32_file_dialog(path, cap, 0);
}

static b32 win32_save_dialog(char *path, u64 cap)
{
    return win32_file_dialog(path, cap, 1);
}

static UnsavedChoice win32_confirm_unsaved(const char *name)
{
    char message[EDITOR_PATH_MAX + 64];
    snprintf(message, sizeof(message), "Save changes to %s?", name);
    switch (MessageBoxA(g_window, message, "Editor", MB_YESNOCANCEL | MB_ICONWARNING)) {
        case IDYES: return UNSAVED_SAVE;
        case IDNO:  return UNSAVED_DISCARD;
        default:    return UNSAVED_CANCEL;
    }
}

static b32 win32_set_clipboard(const u8 *text, u64 len)
{
    if (len > MEGABYTES(256)) return 0;

    // '\n' -> "\r\n", then UTF-8 -> UTF-16 for CF_UNICODETEXT.
    u64 newlines = 0;
    for (u64 i = 0; i < len; i++) newlines += text[i] == '\n';
    u64 crlf_len = len + newlines;
    char *crlf = malloc(crlf_len + 1);
    if (!crlf) return 0;
    u64 j = 0;
    for (u64 i = 0; i < len; i++) {
        if (text[i] == '\n') crlf[j++] = '\r';
        crlf[j++] = (char)text[i];
    }

    int wide_len = crlf_len ? MultiByteToWideChar(CP_UTF8, 0, crlf, (int)crlf_len, 0, 0) : 0;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, ((SIZE_T)wide_len + 1) * sizeof(WCHAR));
    b32 ok = 0;
    if (memory) {
        WCHAR *wide = GlobalLock(memory);
        if (wide_len) MultiByteToWideChar(CP_UTF8, 0, crlf, (int)crlf_len, wide, wide_len);
        wide[wide_len] = 0;
        GlobalUnlock(memory);
        if (OpenClipboard(g_window)) {
            EmptyClipboard();
            ok = SetClipboardData(CF_UNICODETEXT, memory) != 0; // the clipboard now owns memory
            CloseClipboard();
        }
        if (!ok) GlobalFree(memory);
    }
    free(crlf);
    return ok;
}

static u8 *g_clipboard_text; // the last text handed out by win32_get_clipboard

static const u8 *win32_get_clipboard(u64 *len)
{
    *len = 0;
    free(g_clipboard_text);
    g_clipboard_text = 0;
    if (!OpenClipboard(g_window)) return 0;

    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    const WCHAR *wide = data ? GlobalLock(data) : 0;
    if (wide) {
        // UTF-16 -> UTF-8 (n includes the terminator), then "\r\n" -> '\n'.
        int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, 0, 0, 0, 0);
        u8 *text = n > 0 ? malloc((size_t)n) : 0;
        if (text) {
            WideCharToMultiByte(CP_UTF8, 0, wide, -1, (char *)text, n, 0, 0);
            u64 w = 0;
            for (int r = 0; r < n - 1; r++) {
                if (text[r] == '\r' && text[r + 1] == '\n') continue;
                text[w++] = text[r];
            }
            *len = w;
            g_clipboard_text = text;
        }
        GlobalUnlock(data);
    }
    CloseClipboard();
    return g_clipboard_text;
}

static void win32_set_title(const char *title)
{
    SetWindowTextA(g_window, title);
}

static HANDLE g_console; // stderr of the launching console, for --selftest

static void win32_debug_print(const char *text)
{
    OutputDebugStringA(text);
    if (g_console) {
        DWORD written;
        WriteFile(g_console, text, (DWORD)str_len(text), &written, 0);
    }
}

//
// Window
//

static void win32_enable_dpi_awareness(void)
{
    HMODULE user32 = GetModuleHandleA("user32.dll");
    SetProcessDpiAwarenessContextFn set_context =
        (SetProcessDpiAwarenessContextFn)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
    // (HANDLE)-4 is DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 (Windows 10 1703+).
    if (!set_context || !set_context((HANDLE)-4)) SetProcessDPIAware();
    g_get_dpi_for_window = (GetDpiForWindowFn)GetProcAddress(user32, "GetDpiForWindow");
}

static int win32_window_dpi(HWND window)
{
    if (g_get_dpi_for_window) return (int)g_get_dpi_for_window(window);
    HDC dc = GetDC(0);
    int dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(0, dc);
    return dpi;
}

static void win32_resize_bitmap(int width, int height)
{
    if (g_bitmap.pixels) VirtualFree(g_bitmap.pixels, 0, MEM_RELEASE);
    g_bitmap.pixels = 0;
    g_bitmap.width = 0;
    g_bitmap.height = 0;
    if (width <= 0 || height <= 0) return;

    g_bitmap.pixels = VirtualAlloc(0, (SIZE_T)width * (SIZE_T)height * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_bitmap.pixels) return;
    g_bitmap.width = width;
    g_bitmap.height = height;
    g_bitmap.pitch = width;

    BITMAPINFOHEADER *header = &g_bitmap_info.bmiHeader;
    header->biSize = sizeof(*header);
    header->biWidth = width;
    header->biHeight = -height; // top-down
    header->biPlanes = 1;
    header->biBitCount = 32;
    header->biCompression = BI_RGB;
}

// Returns Output.handled.
static b32 win32_send(Input *input)
{
    if (!g_editor_ready) return 0;
    if (g_in_editor) {
        // Re-entered from a modal dialog's message loop; only a resize is worth replaying.
        if (input->kind == INPUT_RESIZE) g_resize_pending = 1;
        return 0;
    }

    g_in_editor = 1;
    Output out = editor_update_and_render(&g_memory, &g_platform, input, &g_bitmap);
    g_in_editor = 0;

    if (out.quit) {
        DestroyWindow(g_window);
        return 1;
    }
    if (out.redraw) InvalidateRect(g_window, 0, FALSE);
    if (g_resize_pending) {
        Input resize = {0};
        resize.kind = INPUT_RESIZE;
        g_resize_pending = 0;
        win32_send(&resize);
    }
    return out.handled;
}

// %APPDATA%\Editor\keybinds.txt, creating the folder. Returns 0 if there is no APPDATA.
static b32 win32_config_path(char *path, u64 cap)
{
    char dir[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("APPDATA", dir, sizeof(dir));
    if (n == 0 || n >= sizeof(dir) - 16) return 0;
    str_append(dir, sizeof(dir), "\\Editor");
    if (!CreateDirectoryA(dir, 0) && GetLastError() != ERROR_ALREADY_EXISTS) return 0;
    str_copy(path, cap, dir);
    str_append(path, cap, "\\keybinds.txt");
    return 1;
}

static u32 win32_mods(void)
{
    u32 mods = 0;
    // Shift and Alt are swapped: this machine reports the physical keys the other way round.
    if (GetKeyState(VK_MENU) & 0x8000) mods |= MOD_SHIFT;
    if (GetKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CTRL;
    if (GetKeyState(VK_SHIFT) & 0x8000) mods |= MOD_ALT;
    return mods;
}

static Key win32_map_key(WPARAM vk, u32 mods)
{
    switch (vk) {
        case VK_LEFT:   return KEY_LEFT;
        case VK_RIGHT:  return KEY_RIGHT;
        case VK_UP:     return KEY_UP;
        case VK_DOWN:   return KEY_DOWN;
        case VK_HOME:   return KEY_HOME;
        case VK_END:    return KEY_END;
        case VK_PRIOR:  return KEY_PAGE_UP;
        case VK_NEXT:   return KEY_PAGE_DOWN;
        case VK_BACK:   return KEY_BACKSPACE;
        case VK_DELETE: return KEY_DELETE;
        case VK_RETURN: return KEY_ENTER;
        case VK_TAB:    return KEY_TAB;
        case VK_ESCAPE: return KEY_ESCAPE;
        default: break;
    }
    if (vk >= VK_F1 && vk <= VK_F12) return (Key)(KEY_F1 + (int)(vk - VK_F1));

    // Character keys are shortcuts only with Ctrl or Alt. AltGr arrives as Ctrl+Alt and must keep typing.
    b32 ctrl = (mods & MOD_CTRL) != 0;
    b32 alt = (mods & MOD_ALT) != 0;
    if (ctrl == alt) return KEY_NONE;
    if (vk >= 'A' && vk <= 'Z') return (Key)(KEY_A + (int)(vk - 'A'));
    if (vk >= '0' && vk <= '9') return (Key)(KEY_0 + (int)(vk - '0'));
    switch (vk) {
        case VK_SPACE:      return KEY_SPACE;
        case VK_OEM_COMMA:  return KEY_COMMA;
        case VK_OEM_PERIOD: return KEY_PERIOD;
        case VK_OEM_MINUS:  return KEY_MINUS;
        case VK_OEM_PLUS:   return KEY_EQUALS;
        default:            return KEY_NONE;
    }
}

static LRESULT CALLBACK win32_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    Input input = {0};
    switch (message) {
        case WM_CLOSE:
            input.kind = INPUT_CLOSE_REQUEST;
            win32_send(&input);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        case WM_SIZE:
            win32_resize_bitmap(LOWORD(lparam), HIWORD(lparam));
            input.kind = INPUT_RESIZE;
            win32_send(&input);
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT paint;
            HDC dc = BeginPaint(window, &paint);
            if (g_bitmap.pixels) {
                StretchDIBits(dc, 0, 0, g_bitmap.width, g_bitmap.height, 0, 0, g_bitmap.width, g_bitmap.height,
                              g_bitmap.pixels, &g_bitmap_info, DIB_RGB_COLORS, SRCCOPY);
            }
            EndPaint(window, &paint);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: // Alt chords and F10
            input.kind = INPUT_KEY;
            input.mods = win32_mods();
            input.key = win32_map_key(wparam, input.mods);
#ifdef EDITOR_KEYLOG // TEMP
            { FILE *log = fopen(EDITOR_KEYLOG, "a"); if (log) { fprintf(log, "msg=%x vk=%x lparam=%08llx mods=%u key=%d shift=%04x ctrl=%04x alt=%04x lshift=%04x rshift=%04x\n", message, (unsigned)wparam, (unsigned long long)lparam, input.mods, (int)input.key, (unsigned short)GetKeyState(VK_SHIFT), (unsigned short)GetKeyState(VK_CONTROL), (unsigned short)GetKeyState(VK_MENU), (unsigned short)GetKeyState(VK_LSHIFT), (unsigned short)GetKeyState(VK_RSHIFT)); fclose(log); } }
#endif
            if (input.key == KEY_NONE || !win32_send(&input)) break; // unbound: Windows default (Alt+F4)
            // The chord ran a command; drop the character TranslateMessage queued for it (Ctrl+Space, Alt+x).
            MSG queued;
            while (PeekMessageA(&queued, window, WM_CHAR, WM_CHAR, PM_REMOVE)) {}
            while (PeekMessageA(&queued, window, WM_SYSCHAR, WM_SYSCHAR, PM_REMOVE)) {}
            return 0;

        case WM_CHAR:
            // Enter, Tab and Backspace come through WM_KEYDOWN; control characters are dropped here.
            if (wparam >= 32 && wparam != 127) {
                input.kind = INPUT_CHAR;
                input.codepoint = (u32)wparam;
                win32_send(&input);
            }
            return 0;

        case WM_MOUSEWHEEL: {
            g_wheel_remainder += GET_WHEEL_DELTA_WPARAM(wparam);
            int lines = g_wheel_remainder * 3 / WHEEL_DELTA; // 3 lines per notch
            if (lines) {
                g_wheel_remainder -= lines * WHEEL_DELTA / 3;
                input.kind = INPUT_WHEEL;
                input.wheel_lines = lines;
                win32_send(&input);
            }
            return 0;
        }

        case WM_DPICHANGED: {
            RECT *suggested = (RECT *)lparam;
            SetWindowPos(window, 0, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            input.kind = INPUT_DPI_CHANGED;
            input.dpi = HIWORD(wparam);
            win32_send(&input);
            return 0;
        }

        default:
            break;
    }
    return DefWindowProcA(window, message, wparam, lparam);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE prev_instance, LPSTR command_line, int show_command)
{
    (void)prev_instance;
    (void)command_line;

    const char *path = 0;
    b32 self_test = 0;
    for (int i = 1; i < __argc; i++) {
        if (strcmp(__argv[i], "--selftest") == 0) self_test = 1;
        else path = __argv[i];
    }

    g_memory.size = EDITOR_MEMORY_SIZE;
    g_memory.base = VirtualAlloc(0, (SIZE_T)g_memory.size, MEM_RESERVE, PAGE_READWRITE);
    if (!g_memory.base) {
        MessageBoxA(0, "Could not reserve memory.", "Editor", MB_ICONERROR);
        return 1;
    }

    g_platform.commit = win32_commit;
    g_platform.file_size = win32_file_size;
    g_platform.read_file = win32_read_file;
    g_platform.begin_write = win32_begin_write;
    g_platform.write = win32_write;
    g_platform.end_write = win32_end_write;
    g_platform.list_directory = win32_list_directory;
    g_platform.open_dialog = win32_open_dialog;
    g_platform.save_dialog = win32_save_dialog;
    g_platform.confirm_unsaved = win32_confirm_unsaved;
    g_platform.set_clipboard = win32_set_clipboard;
    g_platform.get_clipboard = win32_get_clipboard;
    g_platform.set_title = win32_set_title;
    g_platform.debug_print = win32_debug_print;

    // Exit code is the result, for scripts: 0 = pass.
    if (self_test) {
        // stderr is inherited when redirected; otherwise borrow the launching console's.
        HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
        if ((!error || error == INVALID_HANDLE_VALUE) && AttachConsole(ATTACH_PARENT_PROCESS)) {
            error = GetStdHandle(STD_ERROR_HANDLE);
        }
        if (error && error != INVALID_HANDLE_VALUE) g_console = error;
        b32 passed = editor_self_test(&g_memory, &g_platform);
        win32_debug_print(passed ? "self-test passed\n" : "self-test FAILED\n");
        return passed ? 0 : 1;
    }
#if EDITOR_DEBUG
    // Debug builds check the core on every start; failures are listed in the debugger output.
    if (!editor_self_test(&g_memory, &g_platform)) MessageBoxA(0, "Self-test failed.", "Editor", MB_ICONWARNING);
#endif

    win32_enable_dpi_awareness();

    WNDCLASSEXA window_class = {0};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = win32_window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorA(0, (LPCSTR)IDC_IBEAM);
    window_class.hIcon = LoadIconA(0, (LPCSTR)IDI_APPLICATION);
    window_class.lpszClassName = "EditorWindowClass";
    if (!RegisterClassExA(&window_class)) return 1;

    g_window = CreateWindowExA(0, window_class.lpszClassName, "Editor", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                               CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, 0, 0, instance, 0);
    if (!g_window) return 1;

    int dpi = win32_window_dpi(g_window);
    SetWindowPos(g_window, 0, 0, 0, MulDiv(1000, dpi, 96), MulDiv(700, dpi, 96),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    RECT client;
    GetClientRect(g_window, &client);
    win32_resize_bitmap(client.right, client.bottom);

    char config_path[EDITOR_PATH_MAX];
    b32 has_config = win32_config_path(config_path, sizeof(config_path));

    // `editor.exe C:\code\proj` or `editor.exe .` opens a folder instead of a file.
    char folder[EDITOR_PATH_MAX];
    b32 has_folder = 0;
    if (path) {
        DWORD attributes = GetFileAttributesA(path);
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            DWORD n = GetFullPathNameA(path, sizeof(folder), folder, 0);
            has_folder = n > 0 && n < sizeof(folder);
            path = 0;
        }
    }

    g_editor_ready = 1;
    Input init = {0};
    init.kind = INPUT_INIT;
    init.dpi = dpi;
    init.path = path;
    init.config_path = has_config ? config_path : 0;
    init.folder = has_folder ? folder : 0;
    win32_send(&init);

    ShowWindow(g_window, show_command);

    MSG message;
    while (GetMessageA(&message, 0, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
    return 0;
}
