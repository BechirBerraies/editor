// editor.h - the contract between the platform layer and the editor core.
// The core only sees what is declared here; everything OS-specific goes through Platform.
#ifndef EDITOR_H
#define EDITOR_H

#include <stdint.h>

typedef uint8_t  u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t  s32;
typedef int64_t  s64;
typedef int      b32;

#define KILOBYTES(n) ((u64)(n) * 1024)
#define MEGABYTES(n) (KILOBYTES(n) * 1024)
#define GIGABYTES(n) (MEGABYTES(n) * 1024)

// Address space the platform reserves for the editor (committed on demand).
#define EDITOR_MEMORY_SIZE GIGABYTES(2)
#define EDITOR_PATH_MAX 1024

typedef struct Memory {
    void *base;
    u64 size;
    b32 initialized;
} Memory;

// 32-bit pixels, 0x00RRGGBB, top-down rows.
typedef struct Bitmap {
    u32 *pixels;
    int width;
    int height;
    int pitch; // in pixels
} Bitmap;

typedef enum Key {
    KEY_NONE,
    KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN,
    KEY_HOME, KEY_END, KEY_PAGE_UP, KEY_PAGE_DOWN,
    KEY_BACKSPACE, KEY_DELETE, KEY_ENTER, KEY_TAB, KEY_ESCAPE,
    // Character keys below are only sent with Ctrl or Alt held; plain typing arrives as INPUT_CHAR.
    KEY_SPACE, KEY_COMMA, KEY_PERIOD, KEY_MINUS, KEY_EQUALS,
    KEY_A, KEY_Z = KEY_A + 25,
    KEY_0, KEY_9 = KEY_0 + 9,
    KEY_F1, KEY_F12 = KEY_F1 + 11,
    KEY_COUNT
} Key;

enum { MOD_SHIFT = 1, MOD_CTRL = 2, MOD_ALT = 4 };

typedef enum InputKind {
    INPUT_INIT,          // first call: dpi, path
    INPUT_CHAR,          // codepoint
    INPUT_KEY,           // key, mods
    INPUT_WHEEL,         // wheel_lines, positive scrolls up
    INPUT_RESIZE,
    INPUT_DPI_CHANGED,   // dpi
    INPUT_CLOSE_REQUEST,
} InputKind;

typedef struct Input {
    InputKind kind;
    u32 mods;
    Key key;
    u32 codepoint;
    int wheel_lines;
    int dpi;
    const char *path;        // INPUT_INIT: file from the command line, or NULL
    const char *config_path; // INPUT_INIT: keybinds file (its folder exists), or NULL
    const char *folder;      // INPUT_INIT: absolute path of a folder opened from the command line, or NULL
} Input;

typedef void (*DirEntryFn)(void *ctx, const char *name, b32 is_dir);

typedef enum UnsavedChoice { UNSAVED_SAVE, UNSAVED_DISCARD, UNSAVED_CANCEL } UnsavedChoice;

typedef struct Platform {
    b32 (*commit)(void *base, u64 size);
    s64 (*file_size)(const char *path); // -1 if the file does not exist, -2 on any other error
    b32 (*read_file)(const char *path, void *dest, u64 size);
    // Atomic save: writes go to "<path>.tmp", end_write(keep=1) replaces <path> with it.
    void *(*begin_write)(const char *path);
    b32 (*write)(void *file, const void *data, u64 size);
    b32 (*end_write)(void *file, b32 keep);
    // Calls on_entry for each entry of dir (not recursive, no "." or ".."). 0 if dir can't be listed.
    b32 (*list_directory)(const char *dir, DirEntryFn on_entry, void *ctx);
    b32 (*open_dialog)(char *path, u64 cap);
    b32 (*save_dialog)(char *path, u64 cap); // path holds the suggestion on entry
    UnsavedChoice (*confirm_unsaved)(const char *name);
    // Clipboard text is UTF-8 with '\n' line endings; the platform converts to and from the OS format.
    b32 (*set_clipboard)(const u8 *text, u64 len);
    const u8 *(*get_clipboard)(u64 *len); // valid until the next call; NULL when there is no text
    void (*set_title)(const char *title);
    void (*debug_print)(const char *text);
} Platform;

typedef struct Output {
    b32 redraw;
    b32 quit;
    b32 handled; // INPUT_KEY: the key was bound; otherwise the platform may apply its default (Alt+F4)
} Output;

Output editor_update_and_render(Memory *memory, Platform *platform, Input *input, Bitmap *bitmap);
b32 editor_self_test(Memory *memory, Platform *platform);

#endif
