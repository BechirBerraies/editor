// editor.c - the platform-independent editor core. Never includes windows.h:
// memory, files, dialogs and the window title all go through Platform.
#include "editor.h"
#include "editor_arena.h"

#include <string.h>
#include <stdlib.h> // qsort

#ifndef EDITOR_DEBUG
#define EDITOR_DEBUG 0
#endif

#if EDITOR_DEBUG
#define EDITOR_ASSERT(x) do { if (!(x)) __debugbreak(); } while (0)
#else
#define EDITOR_ASSERT(x) ((void)0)
#endif

// stb_truetype allocates from the font arena; nothing is freed until the next bake resets it.
static Arena *g_stb_arena;
static void *stb_arena_alloc(u64 size) { return arena_push(g_stb_arena, size); }

#define STBTT_malloc(size, user) ((void)(user), stb_arena_alloc(size))
#define STBTT_free(ptr, user) ((void)(ptr), (void)(user))
#define STB_TRUETYPE_IMPLEMENTATION
#pragma warning(push, 0)
#include "../third_party/stb_truetype.h"
#pragma warning(pop)

// The editor owns its memory: all allocation goes through arenas.
#define malloc  DO_NOT_USE_MALLOC
#define realloc DO_NOT_USE_REALLOC
#define free    DO_NOT_USE_FREE

#define EDITOR_FONT_PATH "C:\\Windows\\Fonts\\consola.ttf"
#define EDITOR_FONT_PX   18 // at 96 DPI
#define EDITOR_TAB_WIDTH 4

#define THEME_BACKGROUND   0x1E1E1E
#define THEME_TEXT         0xD4D4D4
#define THEME_CURRENT_LINE 0x2A2A2A
#define THEME_GUTTER_TEXT  0x858585
#define THEME_CURSOR       0xFFCC00
#define THEME_SELECTION    0x264F78
#define THEME_UNKNOWN_BYTE 0x808080
#define THEME_PICKER_BG    0x252526
#define THEME_PICKER_EDGE  0x454545
#define THEME_PICKER_SEL   0x094771

#define PERMANENT_SIZE    MEGABYTES(16)
#define FONT_SIZE         MEGABYTES(16)
#define DOCUMENT_SIZE     GIGABYTES(1)
#define UNDO_RECORDS_SIZE MEGABYTES(256)
#define UNDO_TEXT_SIZE    MEGABYTES(512)
#define TRANSIENT_SIZE    MEGABYTES(64)
#define PICKER_SIZE       MEGABYTES(64)

typedef char memory_layout_fits[(PERMANENT_SIZE + FONT_SIZE + DOCUMENT_SIZE + UNDO_RECORDS_SIZE +
                                 UNDO_TEXT_SIZE + TRANSIENT_SIZE + PICKER_SIZE) <= EDITOR_MEMORY_SIZE ? 1 : -1];

#define PICKER_MAX_FILES 20000
#define PICKER_MAX_DEPTH 32
#define PICKER_MAX_ROWS  15

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

typedef struct Rect { int x0, y0, x1, y1; } Rect;

typedef struct GapBuffer {
    Arena *arena; // the buffer is the arena's whole committed range
    u8 *data;
    u64 cap;
    u64 gap_start;
    u64 gap_end;
} GapBuffer;

enum { EDIT_INSERT = 1, EDIT_DELETE = 2 };

typedef struct Edit {
    u64 kind;
    u64 pos;
    u64 len;
    u64 text_offset;   // into the undo_text arena
    u64 cursor_before; // where undo puts the cursor
    u64 joined;        // undone and redone together with the edit before it
} Edit;

typedef struct PickerMatch {
    s32 score;
    u32 index; // into picker_files
} PickerMatch;

typedef struct Font {
    b32 loaded;
    int px;
    int cell_w;
    int line_h;
    int ascent; // baseline offset from the top of a line
    u8 *atlas;
    int atlas_w;
    stbtt_bakedchar glyphs[95]; // ASCII 32..126
} Font;

typedef struct EditorState {
    Platform *platform;

    Arena permanent;
    Arena font_arena;
    Arena document;
    Arena undo_records;
    Arena undo_text;
    Arena transient; // reset on every update

    u8 *font_file;
    Font font;
    int dpi;

    GapBuffer text;
    u64 cursor;
    u64 anchor; // the selection is the text between anchor and cursor; none when they are equal
    int desired_col;
    b32 has_desired_col;
    u64 top_line;

    // Layout, recomputed every update.
    u64 total_lines;
    int pad;
    int text_x;
    int text_bottom;
    int visible_lines;
    int gutter_digits;

    // Linear undo: edits[0, undo_index) are applied, edits[undo_index, edit_count) can be redone.
    Edit *edits;
    u64 edit_count;
    u64 undo_index;
    s64 saved_index; // undo_index at the last save, -1 if unreachable
    b32 undo_break;  // the next edit starts a new undo group

    char path[EDITOR_PATH_MAX]; // empty when untitled
    b32 crlf;
    b32 bom;

    // keymap[mods][key] is a Command; mods is any combination of MOD_SHIFT | MOD_CTRL | MOD_ALT.
    u8 keymap[8][KEY_COUNT];
    char config_path[EDITOR_PATH_MAX]; // empty when the platform has no config folder
    b32 quit_requested;

    char folder[EDITOR_PATH_MAX]; // workspace folder, empty when none was opened

    // File picker: files[] are paths relative to root, rebuilt on every open.
    Arena picker_arena;
    b32 picker_open;
    char picker_root[EDITOR_PATH_MAX];
    char **picker_files;
    u32 picker_file_count;
    b32 picker_truncated;
    PickerMatch *picker_matches;
    u32 picker_match_count;
    u32 picker_selected;
    u32 picker_top;
    char picker_query[128];

    char status[256];
    char title[EDITOR_PATH_MAX + 32];
} EditorState;

//
// Strings and paths
//

static u64 str_len(const char *s)
{
    u64 n = 0;
    while (s[n]) n++;
    return n;
}

static void str_copy(char *dst, u64 cap, const char *src)
{
    u64 i = 0;
    for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
    if (cap) dst[i] = 0;
}

static void str_append(char *dst, u64 cap, const char *src)
{
    u64 n = str_len(dst);
    if (n < cap) str_copy(dst + n, cap - n, src);
}

static void str_append_u64(char *dst, u64 cap, u64 value)
{
    char digits[24];
    int i = (int)sizeof(digits) - 1;
    digits[i] = 0;
    do {
        digits[--i] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    str_append(dst, cap, digits + i);
}

static u8 to_lower(u8 c)
{
    return c >= 'A' && c <= 'Z' ? (u8)(c + ('a' - 'A')) : c;
}

// Case-insensitive match of the span [text, text + len) against a whole lowercase name.
static b32 span_is(const u8 *text, u64 len, const char *name)
{
    u64 i = 0;
    for (; i < len && name[i]; i++) {
        if (to_lower(text[i]) != (u8)name[i]) return 0;
    }
    return i == len && name[i] == 0;
}

static b32 is_separator(char c)
{
    return c == '/' || c == '\\';
}

static const char *path_name(const char *path)
{
    const char *name = path;
    for (const char *p = path; *p; p++) {
        if (is_separator(*p)) name = p + 1;
    }
    return name;
}

// Windows paths: case-insensitive, '/' and '\' are the same.
static b32 path_equal(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        u8 ca = to_lower((u8)*a), cb = to_lower((u8)*b);
        if (ca == '/') ca = '\\';
        if (cb == '/') cb = '\\';
        if (ca != cb) return 0;
    }
    return *a == *b;
}

static void set_status(EditorState *s, const char *message, const char *detail)
{
    str_copy(s->status, sizeof(s->status), message);
    if (detail) str_append(s->status, sizeof(s->status), detail);
}

static const char *document_name(EditorState *s)
{
    return s->path[0] ? path_name(s->path) : "Untitled";
}

//
// Gap buffer
//

static u64 gb_length(GapBuffer *b)
{
    return b->cap - (b->gap_end - b->gap_start);
}

static u8 gb_at(GapBuffer *b, u64 pos)
{
    EDITOR_ASSERT(pos < gb_length(b));
    return pos < b->gap_start ? b->data[pos] : b->data[pos + (b->gap_end - b->gap_start)];
}

static void gb_move_gap(GapBuffer *b, u64 pos)
{
    EDITOR_ASSERT(pos <= gb_length(b));
    if (pos < b->gap_start) {
        u64 n = b->gap_start - pos;
        memmove(b->data + b->gap_end - n, b->data + pos, n);
        b->gap_start -= n;
        b->gap_end -= n;
    } else if (pos > b->gap_start) {
        u64 n = pos - b->gap_start;
        memmove(b->data + b->gap_start, b->data + b->gap_end, n);
        b->gap_start += n;
        b->gap_end += n;
    }
}

// Grows in place: commit more of the arena and slide the post-gap text to the new end.
static b32 gb_reserve(GapBuffer *b, u64 n)
{
    if (b->gap_end - b->gap_start >= n) return 1;

    u64 needed = gb_length(b) + n;
    u64 new_cap = MAX(b->cap * 2, KILOBYTES(64));
    while (new_cap < needed) new_cap *= 2;
    new_cap = MIN(new_cap, b->arena->reserved);
    if (new_cap < needed) return 0;
    if (!arena_ensure_committed(b->arena, new_cap)) return 0;

    u64 tail = b->cap - b->gap_end;
    memmove(b->data + new_cap - tail, b->data + b->gap_end, tail);
    b->gap_end = new_cap - tail;
    b->cap = new_cap;
    return 1;
}

static b32 gb_insert(GapBuffer *b, u64 pos, const u8 *text, u64 n)
{
    if (!gb_reserve(b, n)) return 0;
    gb_move_gap(b, pos);
    memcpy(b->data + b->gap_start, text, n);
    b->gap_start += n;
    return 1;
}

static void gb_delete(GapBuffer *b, u64 pos, u64 n)
{
    EDITOR_ASSERT(pos + n <= gb_length(b));
    gb_move_gap(b, pos);
    b->gap_end += n;
}

static void gb_copy(GapBuffer *b, u64 pos, u64 n, u8 *dest)
{
    for (u64 i = 0; i < n; i++) dest[i] = gb_at(b, pos + i);
}

static u64 gb_count_newlines(GapBuffer *b, u64 from, u64 to)
{
    u64 count = 0;
    u64 gap = b->gap_end - b->gap_start;
    u64 i = from;
    for (; i < to && i < b->gap_start; i++) count += b->data[i] == '\n';
    for (; i < to; i++) count += b->data[i + gap] == '\n';
    return count;
}

static u64 gb_line_to_pos(GapBuffer *b, u64 line)
{
    u64 len = gb_length(b);
    u64 pos = 0;
    while (line && pos < len) {
        if (gb_at(b, pos) == '\n') line--;
        pos++;
    }
    return pos;
}

//
// Lines and columns. Columns are visual: a tab advances to the next tab stop.
//

static u64 line_start(GapBuffer *b, u64 pos)
{
    while (pos > 0 && gb_at(b, pos - 1) != '\n') pos--;
    return pos;
}

static u64 line_end(GapBuffer *b, u64 pos)
{
    u64 len = gb_length(b);
    while (pos < len && gb_at(b, pos) != '\n') pos++;
    return pos;
}

static int char_width(u8 c, int col)
{
    return c == '\t' ? EDITOR_TAB_WIDTH - col % EDITOR_TAB_WIDTH : 1;
}

static int visual_col(GapBuffer *b, u64 start, u64 pos)
{
    int col = 0;
    for (u64 i = start; i < pos; i++) col += char_width(gb_at(b, i), col);
    return col;
}

static u64 pos_at_col(GapBuffer *b, u64 start, int target)
{
    u64 end = line_end(b, start);
    u64 pos = start;
    int col = 0;
    while (pos < end) {
        int w = char_width(gb_at(b, pos), col);
        if (col + w > target) break;
        col += w;
        pos++;
    }
    return pos;
}

//
// Undo
//

static b32 is_dirty(EditorState *s)
{
    return (s64)s->undo_index != s->saved_index;
}

static void mark_saved(EditorState *s)
{
    s->saved_index = (s64)s->undo_index;
    s->undo_break = 1;
}

static void undo_clear(EditorState *s)
{
    s->edit_count = 0;
    s->undo_index = 0;
    s->undo_text.used = 0;
    s->saved_index = -1;
    s->undo_break = 1;
}

static b32 undo_append_text(Arena *a, const u8 *text, u64 n)
{
    if (!arena_ensure_committed(a, a->used + n)) return 0;
    memcpy(a->base + a->used, text, n);
    a->used += n;
    return 1;
}

// Records an edit that was just applied, merging it into the previous one when it continues it.
static void undo_push(EditorState *s, u64 kind, u64 pos, const u8 *text, u64 n, u64 cursor_before)
{
    // A new edit drops everything that could have been redone.
    if (s->edit_count > s->undo_index) {
        s->undo_text.used = s->edits[s->undo_index].text_offset;
        s->edit_count = s->undo_index;
        if (s->saved_index > (s64)s->undo_index) s->saved_index = -1;
    }

    Edit *last = s->undo_index ? &s->edits[s->undo_index - 1] : 0;
    b32 can_merge = last && !s->undo_break && last->kind == kind && s->saved_index != (s64)s->undo_index;
    b32 ok;

    if (can_merge && kind == EDIT_INSERT && last->pos + last->len == pos) {
        ok = undo_append_text(&s->undo_text, text, n);
        if (ok) last->len += n;
    } else if (can_merge && kind == EDIT_DELETE && last->pos == pos) {
        // Delete key: the text came from after the previous deletion.
        ok = undo_append_text(&s->undo_text, text, n);
        if (ok) last->len += n;
    } else if (can_merge && kind == EDIT_DELETE && pos + n == last->pos) {
        // Backspace: the text came from before; the last record's text sits at the end of the arena.
        Arena *a = &s->undo_text;
        ok = arena_ensure_committed(a, a->used + n);
        if (ok) {
            u8 *t = a->base + last->text_offset;
            memmove(t + n, t, last->len);
            memcpy(t, text, n);
            a->used += n;
            last->pos = pos;
            last->len += n;
        }
    } else {
        u64 offset = s->undo_text.used;
        ok = arena_ensure_committed(&s->undo_records, (s->edit_count + 1) * sizeof(Edit)) &&
             undo_append_text(&s->undo_text, text, n);
        if (ok) {
            Edit *e = &s->edits[s->edit_count++];
            e->kind = kind;
            e->pos = pos;
            e->len = n;
            e->text_offset = offset;
            e->cursor_before = cursor_before;
            e->joined = 0;
            s->undo_index = s->edit_count;
        }
    }

    s->undo_break = 0;
    if (!ok) {
        undo_clear(s);
        set_status(s, "Undo history full, history cleared", 0);
    }
}

static void undo(EditorState *s)
{
    if (s->undo_index == 0) {
        set_status(s, "Nothing to undo", 0);
        return;
    }
    for (;;) {
        Edit *e = &s->edits[s->undo_index - 1];
        if (e->kind == EDIT_INSERT) {
            gb_delete(&s->text, e->pos, e->len);
        } else if (!gb_insert(&s->text, e->pos, s->undo_text.base + e->text_offset, e->len)) {
            set_status(s, "Out of memory", 0);
            return;
        }
        s->undo_index--;
        s->cursor = e->cursor_before;
        if (!e->joined || s->undo_index == 0) break;
    }
    s->anchor = s->cursor;
    s->has_desired_col = 0;
    s->undo_break = 1;
}

static void redo(EditorState *s)
{
    if (s->undo_index == s->edit_count) {
        set_status(s, "Nothing to redo", 0);
        return;
    }
    do {
        Edit *e = &s->edits[s->undo_index];
        if (e->kind == EDIT_INSERT) {
            if (!gb_insert(&s->text, e->pos, s->undo_text.base + e->text_offset, e->len)) {
                set_status(s, "Out of memory", 0);
                return;
            }
            s->cursor = e->pos + e->len;
        } else {
            gb_delete(&s->text, e->pos, e->len);
            s->cursor = e->pos;
        }
        s->undo_index++;
    } while (s->undo_index < s->edit_count && s->edits[s->undo_index].joined);
    s->anchor = s->cursor;
    s->has_desired_col = 0;
    s->undo_break = 1;
}

//
// Selection, editing and cursor movement
//

static b32 has_selection(EditorState *s)
{
    return s->anchor != s->cursor;
}

static void selection_range(EditorState *s, u64 *start, u64 *end)
{
    *start = MIN(s->anchor, s->cursor);
    *end = MAX(s->anchor, s->cursor);
}

static void edit_delete(EditorState *s, u64 pos, u64 n)
{
    u8 *deleted = arena_push(&s->transient, n);
    if (!deleted) {
        set_status(s, "Out of memory", 0);
        return;
    }
    u64 cursor_before = s->cursor;
    gb_copy(&s->text, pos, n, deleted);
    gb_delete(&s->text, pos, n);
    undo_push(s, EDIT_DELETE, pos, deleted, n, cursor_before);
    s->cursor = pos;
    s->anchor = pos;
    s->has_desired_col = 0;
}

// Deleting a selection is its own undo step: it never merges with typing or Backspace.
static b32 delete_selection(EditorState *s)
{
    if (!has_selection(s)) return 0;
    u64 start, end;
    selection_range(s, &start, &end);
    s->undo_break = 1;
    edit_delete(s, start, end - start);
    s->undo_break = 1;
    return 1;
}

// Inserts at the cursor, replacing the selection if there is one.
static void edit_insert(EditorState *s, const u8 *text, u64 n)
{
    delete_selection(s);
    u64 pos = s->cursor;
    if (!gb_insert(&s->text, pos, text, n)) {
        set_status(s, "Out of memory", 0);
        return;
    }
    undo_push(s, EDIT_INSERT, pos, text, n, pos);
    s->cursor = pos + n;
    s->anchor = s->cursor;
    s->has_desired_col = 0;
    if (memchr(text, '\n', n)) s->undo_break = 1; // each line is its own undo step
}

// The closing half of a bracket or quote pair, or 0.
static u8 pair_closer(u8 c)
{
    switch (c) {
        case '(': return ')';
        case '[': return ']';
        case '{': return '}';
        case '"': return '"';
        case '\'': return '\'';
        default: return 0;
    }
}

// The cursor sits between an empty pair: (|) [|] {|} "|" '|'
static b32 cursor_in_empty_pair(EditorState *s)
{
    GapBuffer *b = &s->text;
    if (has_selection(s) || s->cursor == 0 || s->cursor >= gb_length(b)) return 0;
    u8 closer = pair_closer(gb_at(b, s->cursor - 1));
    return closer && gb_at(b, s->cursor) == closer;
}

// In an empty pair both halves go, as one undo step.
static void backspace(EditorState *s)
{
    if (cursor_in_empty_pair(s)) {
        s->undo_break = 1;
        edit_delete(s, s->cursor - 1, 2);
        s->undo_break = 1;
    } else if (!delete_selection(s) && s->cursor > 0) {
        edit_delete(s, s->cursor - 1, 1);
    }
}

static void delete_forward(EditorState *s)
{
    if (!delete_selection(s) && s->cursor < gb_length(&s->text)) edit_delete(s, s->cursor, 1);
}

// Moves without selecting; run_command restores the anchor when Shift extends the selection.
static void move_cursor(EditorState *s, u64 pos)
{
    s->cursor = pos;
    s->anchor = pos;
    s->has_desired_col = 0;
    s->undo_break = 1;
}

static void move_vertical(EditorState *s, int dir)
{
    GapBuffer *b = &s->text;
    u64 start = line_start(b, s->cursor);
    if (!s->has_desired_col) {
        s->desired_col = visual_col(b, start, s->cursor);
        s->has_desired_col = 1;
    }
    if (dir < 0) {
        s->cursor = start == 0 ? 0 : pos_at_col(b, line_start(b, start - 1), s->desired_col);
    } else {
        u64 end = line_end(b, s->cursor);
        s->cursor = end == gb_length(b) ? end : pos_at_col(b, end + 1, s->desired_col);
    }
    s->anchor = s->cursor;
    s->undo_break = 1;
}

static void select_range(EditorState *s, u64 start, u64 end)
{
    s->anchor = start;
    s->cursor = end;
    s->has_desired_col = 0;
    s->undo_break = 1;
}

// The line under the cursor including its newline; repeating it adds the next line.
static void select_line(EditorState *s)
{
    GapBuffer *b = &s->text;
    u64 start = s->cursor, end = s->cursor;
    if (has_selection(s)) selection_range(s, &start, &end);
    start = line_start(b, start);
    end = line_end(b, end);
    if (end < gb_length(b)) end++;
    select_range(s, start, end);
}

static b32 line_is_blank(GapBuffer *b, u64 start)
{
    for (u64 pos = start, end = line_end(b, start); pos < end; pos++) {
        u8 c = gb_at(b, pos);
        if (c != ' ' && c != '\t' && c != '\r') return 0;
    }
    return 1;
}

// A paragraph is a run of non-blank lines between blank lines (on a blank line: the run of blank lines).
static void select_paragraph(EditorState *s)
{
    GapBuffer *b = &s->text;
    u64 len = gb_length(b);
    u64 start = line_start(b, s->cursor);
    b32 blank = line_is_blank(b, start);
    while (start > 0) {
        u64 prev = line_start(b, start - 1);
        if (line_is_blank(b, prev) != blank) break;
        start = prev;
    }
    u64 end = line_end(b, s->cursor);
    while (end < len && line_is_blank(b, end + 1) == blank) end = line_end(b, end + 1);
    if (end < len) end++;
    select_range(s, start, end);
}

// Copies the selection, or the current line when nothing is selected; cut also deletes it.
static void copy_to_clipboard(EditorState *s, b32 cut)
{
    GapBuffer *b = &s->text;
    u64 start, end;
    if (has_selection(s)) {
        selection_range(s, &start, &end);
    } else {
        start = line_start(b, s->cursor);
        end = line_end(b, s->cursor);
        if (end < gb_length(b)) end++;
    }
    if (start == end) return;

    u8 *text = arena_push(&s->transient, end - start);
    if (!text) {
        set_status(s, "Out of memory", 0);
        return;
    }
    gb_copy(b, start, end - start, text);
    if (!s->platform->set_clipboard(text, end - start)) {
        set_status(s, "Could not copy to the clipboard", 0);
        return;
    }
    if (cut) {
        select_range(s, start, end);
        delete_selection(s);
    }
}

// A paste is one undo step, separate from the typing around it.
static void paste(EditorState *s)
{
    u64 n;
    const u8 *text = s->platform->get_clipboard(&n);
    if (!text || !n) return;
    s->undo_break = 1;
    edit_insert(s, text, n);
    s->undo_break = 1;
}

static b32 is_word_char(u8 c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// Wraps the selection in open/close and keeps the wrapped text selected. One undo step.
static void wrap_selection(EditorState *s, u8 open, u8 close)
{
    u64 start, end;
    selection_range(s, &start, &end);
    b32 forward = s->cursor == end;
    s->undo_break = 1;
    if (!gb_insert(&s->text, end, &close, 1)) {
        set_status(s, "Out of memory", 0);
        return;
    }
    undo_push(s, EDIT_INSERT, end, &close, 1, s->cursor);
    if (!gb_insert(&s->text, start, &open, 1)) {
        set_status(s, "Out of memory", 0);
        return;
    }
    undo_push(s, EDIT_INSERT, start, &open, 1, s->cursor);
    if (s->undo_index >= 2) s->edits[s->undo_index - 1].joined = 1;
    s->undo_break = 1;
    s->anchor = forward ? start + 1 : end + 1;
    s->cursor = forward ? end + 1 : start + 1;
    s->has_desired_col = 0;
}

// A typed character, with bracket and quote pairing: an opener wraps the selection, or inserts
// its closer too when the next character is blank, a closing bracket or the end; quotes also need
// a previous character that isn't part of a word or a backslash. Typing the closer that is
// already next just steps over it.
static void type_char(EditorState *s, u8 c)
{
    GapBuffer *b = &s->text;
    u8 closer = pair_closer(c);
    if (closer && has_selection(s)) {
        wrap_selection(s, c, closer);
        return;
    }
    u64 len = gb_length(b);
    u8 next = !has_selection(s) && s->cursor < len ? gb_at(b, s->cursor) : 0;
    u8 prev = !has_selection(s) && s->cursor > 0 ? gb_at(b, s->cursor - 1) : 0;
    b32 is_quote = c == '"' || c == '\'';
    if (next == c && (is_quote || c == ')' || c == ']' || c == '}')) {
        move_cursor(s, s->cursor + 1);
        return;
    }
    b32 pair = closer && (s->cursor == len || next == ' ' || next == '\t' || next == '\r' || next == '\n' ||
                          next == ')' || next == ']' || next == '}');
    if (pair && is_quote && (is_word_char(prev) || prev == '\\')) pair = 0;
    if (pair) {
        u8 text[2] = { c, closer };
        edit_insert(s, text, 2);
        s->cursor--;
        s->anchor = s->cursor;
    } else {
        edit_insert(s, &c, 1);
    }
}

// Enter keeps the indent before the cursor; between brackets it opens an indented block:
// "{|}" becomes "{\n<indent>    |\n<indent>}".
static void newline(EditorState *s)
{
    GapBuffer *b = &s->text;
    delete_selection(s);
    u64 start = line_start(b, s->cursor);
    u64 indent_end = start;
    while (indent_end < s->cursor && (gb_at(b, indent_end) == ' ' || gb_at(b, indent_end) == '\t')) indent_end++;
    u64 indent = indent_end - start;
    u8 prev = s->cursor > 0 ? gb_at(b, s->cursor - 1) : 0;
    u8 next = s->cursor < gb_length(b) ? gb_at(b, s->cursor) : 0;
    b32 block = (prev == '(' || prev == '[' || prev == '{') && next == pair_closer(prev);

    u64 n = 1 + indent + (block ? 4 + 1 + indent : 0);
    u8 *text = arena_push(&s->transient, n);
    if (!text) {
        set_status(s, "Out of memory", 0);
        return;
    }
    text[0] = '\n';
    gb_copy(b, start, indent, text + 1);
    if (block) {
        memcpy(text + 1 + indent, "    \n", 5);
        gb_copy(b, start, indent, text + 1 + indent + 5);
    }
    u64 pos = s->cursor;
    edit_insert(s, text, n);
    if (block) {
        s->cursor = pos + 1 + indent + 4;
        s->anchor = s->cursor;
    }
}

//
// Document lifetime and files
//

static void doc_reset(EditorState *s)
{
    s->text.gap_start = 0;
    s->text.gap_end = s->text.cap;
    s->cursor = 0;
    s->anchor = 0;
    s->has_desired_col = 0;
    s->top_line = 0;
    undo_clear(s);
    s->saved_index = 0;
    s->path[0] = 0;
    s->crlf = 1; // new files use the Windows convention
    s->bom = 0;
}

// Strips a UTF-8 BOM and, for CRLF files, turns CRLF into LF in place. Returns the new length.
static u64 normalize_loaded_text(u8 *d, u64 n, b32 *crlf, b32 *bom)
{
    *bom = n >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF;
    if (*bom) {
        memmove(d, d + 3, n - 3);
        n -= 3;
    }

    // The first line ending decides the style; files without one default to CRLF.
    *crlf = 1;
    for (u64 i = 0; i < n; i++) {
        if (d[i] == '\n') {
            *crlf = i > 0 && d[i - 1] == '\r';
            break;
        }
    }
    if (!*crlf) return n;

    u64 w = 0;
    for (u64 r = 0; r < n; r++) {
        if (d[r] == '\r' && r + 1 < n && d[r + 1] == '\n') continue;
        d[w++] = d[r];
    }
    return w;
}

static b32 load_file(EditorState *s, const char *path, b32 missing_is_new)
{
    Platform *p = s->platform;
    s64 size = p->file_size(path);
    if (size == -1 && missing_is_new) {
        doc_reset(s);
        str_copy(s->path, sizeof(s->path), path);
        set_status(s, "New file", 0);
        return 1;
    }
    if (size < 0) {
        set_status(s, "Could not open ", path_name(path));
        return 0;
    }
    if ((u64)size > s->document.reserved - KILOBYTES(64)) {
        set_status(s, "File too large: ", path_name(path));
        return 0;
    }

    doc_reset(s);
    GapBuffer *b = &s->text;
    if (!gb_reserve(b, (u64)size) || !p->read_file(path, b->data, (u64)size)) {
        doc_reset(s);
        set_status(s, "Could not read ", path_name(path));
        return 0;
    }
    // The empty buffer's gap starts at 0, so the file landed at the front of it.
    b->gap_start = normalize_loaded_text(b->data, (u64)size, &s->crlf, &s->bom);
    str_copy(s->path, sizeof(s->path), path);
    return 1;
}

static b32 write_document(EditorState *s, const char *path)
{
    Platform *p = s->platform;
    void *file = p->begin_write(path);
    if (!file) return 0;

    static const u8 bom[3] = { 0xEF, 0xBB, 0xBF };
    b32 ok = s->bom ? p->write(file, bom, 3) : 1;

    GapBuffer *b = &s->text;
    const u8 *spans[2] = { b->data, b->data + b->gap_end };
    u64 sizes[2] = { b->gap_start, b->cap - b->gap_end };

    if (!s->crlf) {
        for (int i = 0; i < 2 && ok; i++) {
            if (sizes[i]) ok = p->write(file, spans[i], sizes[i]);
        }
    } else {
        u64 chunk_cap = KILOBYTES(64);
        u8 *chunk = arena_push(&s->transient, chunk_cap);
        u64 used = 0;
        if (!chunk) ok = 0;
        for (int i = 0; i < 2 && ok; i++) {
            for (u64 j = 0; j < sizes[i] && ok; j++) {
                if (used + 2 > chunk_cap) {
                    ok = p->write(file, chunk, used);
                    used = 0;
                }
                if (spans[i][j] == '\n') chunk[used++] = '\r';
                chunk[used++] = spans[i][j];
            }
        }
        if (ok && used) ok = p->write(file, chunk, used);
    }

    return p->end_write(file, ok) && ok;
}

static b32 save(EditorState *s, b32 save_as)
{
    char path[EDITOR_PATH_MAX];
    str_copy(path, sizeof(path), s->path);
    if ((save_as || !path[0]) && !s->platform->save_dialog(path, sizeof(path))) return 0;

    if (!write_document(s, path)) {
        set_status(s, "Could not save ", path_name(path));
        return 0;
    }
    str_copy(s->path, sizeof(s->path), path);
    mark_saved(s);
    set_status(s, "Saved", 0);
    return 1;
}

// Returns 1 when the current document may be replaced or closed.
static b32 confirm_discard(EditorState *s)
{
    if (!is_dirty(s)) return 1;
    switch (s->platform->confirm_unsaved(document_name(s))) {
        case UNSAVED_SAVE:    return save(s, 0);
        case UNSAVED_DISCARD: return 1;
        default:              return 0;
    }
}

static void open_with_dialog(EditorState *s)
{
    if (!confirm_discard(s)) return;
    char path[EDITOR_PATH_MAX] = "";
    if (s->platform->open_dialog(path, sizeof(path))) load_file(s, path, 0);
}

//
// Font
//

static int dpi_scale(EditorState *s, int value)
{
    return value * s->dpi / 96;
}

static void font_bake(EditorState *s)
{
    Font *f = &s->font;
    f->loaded = 0;
    f->px = MAX(6, dpi_scale(s, EDITOR_FONT_PX));
    // Fallback metrics so layout still works without a font (everything draws as boxes).
    f->cell_w = MAX(1, f->px * 3 / 5);
    f->line_h = f->px * 5 / 4;
    f->ascent = f->px;
    if (!s->font_file) return;

    g_stb_arena = &s->font_arena;
    stbtt_fontinfo info;
    arena_reset(&s->font_arena);
    if (!stbtt_InitFont(&info, s->font_file, stbtt_GetFontOffsetForIndex(s->font_file, 0))) return;

    for (int size = 256; size <= 4096 && !f->loaded; size *= 2) {
        arena_reset(&s->font_arena);
        u8 *atlas = arena_push(&s->font_arena, (u64)size * (u64)size);
        if (!atlas) break;
        if (stbtt_BakeFontBitmap(s->font_file, 0, (float)f->px, atlas, size, size, 32, 95, f->glyphs) > 0) {
            f->atlas = atlas;
            f->atlas_w = size;
            f->loaded = 1;
        }
    }
    if (!f->loaded) return;

    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &line_gap);
    float scale = stbtt_ScaleForPixelHeight(&info, (float)f->px);
    int leading = dpi_scale(s, 2);
    f->line_h = (int)((float)(ascent - descent + line_gap) * scale + 0.5f) + leading;
    f->ascent = (int)((float)ascent * scale + 0.5f) + leading / 2;
    f->cell_w = MAX(1, (int)(f->glyphs['M' - 32].xadvance + 0.5f));
}

//
// Drawing
//

static Rect rect_clip(Rect a, Bitmap *bm)
{
    Rect r;
    r.x0 = MAX(a.x0, 0);
    r.y0 = MAX(a.y0, 0);
    r.x1 = MIN(a.x1, bm->width);
    r.y1 = MIN(a.y1, bm->height);
    return r;
}

static void draw_rect(Bitmap *bm, Rect clip, int x0, int y0, int x1, int y1, u32 color)
{
    clip = rect_clip(clip, bm);
    x0 = MAX(x0, clip.x0);
    y0 = MAX(y0, clip.y0);
    x1 = MIN(x1, clip.x1);
    y1 = MIN(y1, clip.y1);
    for (int y = y0; y < y1; y++) {
        u32 *row = bm->pixels + y * bm->pitch;
        for (int x = x0; x < x1; x++) row[x] = color;
    }
}

static u32 blend(u32 dst, u32 src, u32 alpha)
{
    u32 inv = 255 - alpha;
    u32 r = (((src >> 16) & 0xFF) * alpha + ((dst >> 16) & 0xFF) * inv) / 255;
    u32 g = (((src >> 8) & 0xFF) * alpha + ((dst >> 8) & 0xFF) * inv) / 255;
    u32 b = ((src & 0xFF) * alpha + (dst & 0xFF) * inv) / 255;
    return (r << 16) | (g << 8) | b;
}

// Bytes the font can't show (non-ASCII, control characters) draw as an outlined box.
static void draw_box(EditorState *s, Bitmap *bm, Rect clip, int x, int baseline)
{
    Font *f = &s->font;
    int inset = MAX(1, f->cell_w / 6);
    int x0 = x + inset, x1 = x + f->cell_w - inset;
    int y0 = baseline - f->px * 7 / 10, y1 = baseline;
    draw_rect(bm, clip, x0, y0, x1, y0 + 1, THEME_UNKNOWN_BYTE);
    draw_rect(bm, clip, x0, y1 - 1, x1, y1, THEME_UNKNOWN_BYTE);
    draw_rect(bm, clip, x0, y0, x0 + 1, y1, THEME_UNKNOWN_BYTE);
    draw_rect(bm, clip, x1 - 1, y0, x1, y1, THEME_UNKNOWN_BYTE);
}

static void draw_glyph(EditorState *s, Bitmap *bm, Rect clip, int x, int baseline, u8 c, u32 color)
{
    Font *f = &s->font;
    if (c == ' ') return;
    if (!f->loaded || c < 32 || c > 126) {
        draw_box(s, bm, clip, x, baseline);
        return;
    }

    clip = rect_clip(clip, bm);
    stbtt_bakedchar *g = &f->glyphs[c - 32];
    int gx = x + (int)g->xoff;
    int gy = baseline + (int)g->yoff;
    int w = g->x1 - g->x0;
    int h = g->y1 - g->y0;
    for (int row = 0; row < h; row++) {
        int py = gy + row;
        if (py < clip.y0 || py >= clip.y1) continue;
        u8 *src = f->atlas + (g->y0 + row) * f->atlas_w + g->x0;
        u32 *dst = bm->pixels + py * bm->pitch;
        for (int col = 0; col < w; col++) {
            int px = gx + col;
            u32 alpha = src[col];
            if (px < clip.x0 || px >= clip.x1 || alpha == 0) continue;
            dst[px] = alpha == 255 ? color : blend(dst[px], color, alpha);
        }
    }
}

static void draw_string(EditorState *s, Bitmap *bm, Rect clip, int x, int baseline, const char *text, u32 color)
{
    for (; *text; text++, x += s->font.cell_w) draw_glyph(s, bm, clip, x, baseline, (u8)*text, color);
}

static void compute_layout(EditorState *s, Bitmap *bm)
{
    Font *f = &s->font;
    s->total_lines = gb_count_newlines(&s->text, 0, gb_length(&s->text)) + 1;
    s->pad = dpi_scale(s, 6);
    s->text_bottom = bm->height;
    s->visible_lines = MAX(1, (s->text_bottom - s->pad) / f->line_h);

    int digits = 1;
    for (u64 n = s->total_lines; n >= 10; n /= 10) digits++;
    s->gutter_digits = MAX(3, digits);
    s->text_x = s->pad + (s->gutter_digits + 2) * f->cell_w;
}

static void scroll_to_cursor(EditorState *s)
{
    u64 line = gb_count_newlines(&s->text, 0, s->cursor);
    u64 visible = (u64)s->visible_lines;
    if (line < s->top_line) s->top_line = line;
    else if (line >= s->top_line + visible) s->top_line = line - visible + 1;
}

static void render(EditorState *s, Bitmap *bm)
{
    Font *f = &s->font;
    GapBuffer *b = &s->text;
    u64 len = gb_length(b);
    Rect full = { 0, 0, bm->width, bm->height };
    Rect lines_clip = { 0, 0, bm->width, s->text_bottom };
    Rect gutter_clip = { 0, 0, s->text_x, s->text_bottom };
    Rect text_clip = { s->text_x, 0, bm->width, s->text_bottom };

    draw_rect(bm, full, 0, 0, bm->width, bm->height, THEME_BACKGROUND);

    u64 cursor_line = gb_count_newlines(b, 0, s->cursor);
    u64 pos = gb_line_to_pos(b, s->top_line);
    u64 sel_start, sel_end;
    selection_range(s, &sel_start, &sel_end);

    // One extra row so a partially visible last line still shows.
    for (int row = 0; row <= s->visible_lines; row++) {
        u64 line = s->top_line + (u64)row;
        int y = s->pad + row * f->line_h;
        if (line >= s->total_lines || y >= s->text_bottom) break;
        int baseline = y + f->ascent;
        b32 is_cursor_line = line == cursor_line;

        if (is_cursor_line) draw_rect(bm, lines_clip, 0, y, bm->width, y + f->line_h, THEME_CURRENT_LINE);

        char number[24] = "";
        str_append_u64(number, sizeof(number), line + 1);
        int number_x = s->pad + (s->gutter_digits - (int)str_len(number)) * f->cell_w;
        draw_string(s, bm, gutter_clip, number_x, baseline, number, is_cursor_line ? THEME_TEXT : THEME_GUTTER_TEXT);

        int col = 0;
        int cursor_x = -1;
        for (;;) {
            int x = s->text_x + col * f->cell_w;
            if (pos == s->cursor) cursor_x = x;
            if (pos >= len) break;
            u8 c = gb_at(b, pos);
            int w = char_width(c, col);
            if (pos >= sel_start && pos < sel_end) {
                int sel_w = c == '\n' ? f->cell_w / 2 : w * f->cell_w; // a selected newline shows as a sliver
                draw_rect(bm, text_clip, x, y, x + sel_w, y + f->line_h, THEME_SELECTION);
            }
            if (c == '\n') break;
            if (x < bm->width && c != '\t') draw_glyph(s, bm, text_clip, x, baseline, c, THEME_TEXT);
            col += w;
            pos++;
        }
        pos++; // past the newline

        if (cursor_x >= 0) {
            draw_rect(bm, text_clip, cursor_x, y, cursor_x + MAX(2, dpi_scale(s, 2)), y + f->line_h, THEME_CURSOR);
        }
    }
}

static void update_title(EditorState *s)
{
    char title[sizeof(s->title)] = "";
    str_append(title, sizeof(title), document_name(s));
    if (is_dirty(s)) str_append(title, sizeof(title), "*");
    if (s->folder[0]) {
        const char *folder_name = path_name(s->folder);
        str_append(title, sizeof(title), " - ");
        str_append(title, sizeof(title), folder_name[0] ? folder_name : s->folder); // "C:\" has no name
    }
    str_append(title, sizeof(title), " - Editor");
    if (strcmp(title, s->title) != 0) {
        str_copy(s->title, sizeof(s->title), title);
        s->platform->set_title(title);
    }
}

//
// Keybinds: commands, the keybinds.txt format and key lookup.
//

#include "editor_keybinds.c"

//
// File picker: fuzzy search over every file under the workspace folder.
//

static const char *picker_skipped_dirs[] = { ".git", ".hg", ".svn", ".vs", "node_modules", "build" };

typedef struct PickerScan {
    EditorState *s;
    char path[EDITOR_PATH_MAX]; // directory being listed
    u64 relative_start;         // where paths relative to the root begin
    int depth;
} PickerScan;

static void picker_on_entry(void *ctx, const char *name, b32 is_dir)
{
    PickerScan *scan = ctx;
    EditorState *s = scan->s;
    if (s->picker_file_count >= PICKER_MAX_FILES) {
        s->picker_truncated = 1;
        return;
    }

    u64 name_len = str_len(name);
    if (is_dir) {
        if (scan->depth >= PICKER_MAX_DEPTH) return;
        for (int i = 0; i < (int)(sizeof(picker_skipped_dirs) / sizeof(picker_skipped_dirs[0])); i++) {
            if (span_is((const u8 *)name, name_len, picker_skipped_dirs[i])) return;
        }
    }

    u64 dir_len = str_len(scan->path);
    u64 at = dir_len;
    if (at && !is_separator(scan->path[at - 1])) scan->path[at++] = '/';
    if (at + name_len + 1 > sizeof(scan->path)) {
        scan->path[dir_len] = 0;
        return;
    }
    memcpy(scan->path + at, name, name_len + 1);

    if (is_dir) {
        scan->depth++;
        s->platform->list_directory(scan->path, picker_on_entry, scan);
        scan->depth--;
    } else {
        const char *relative = scan->path + scan->relative_start;
        u64 len = str_len(relative);
        char *copy = arena_push(&s->picker_arena, len + 1);
        if (copy) {
            memcpy(copy, relative, len + 1);
            s->picker_files[s->picker_file_count++] = copy;
        } else {
            s->picker_truncated = 1;
        }
    }
    scan->path[dir_len] = 0;
}

static b32 is_word_start(const char *path, const char *p)
{
    if (p == path) return 1;
    char prev = p[-1];
    return is_separator(prev) || prev == '_' || prev == '-' || prev == '.' || prev == ' ' ||
           (prev >= 'a' && prev <= 'z' && *p >= 'A' && *p <= 'Z');
}

// Case-insensitive subsequence match: "wined" matches "src/win32_editor.c". Higher is better,
// -1 for no match. Rewards consecutive letters, word starts and hits in the file name.
static s32 fuzzy_score(const char *query, const char *path)
{
    const char *name = path_name(path);
    const char *q = query;
    s32 score = 0;
    b32 prev_matched = 0;
    for (const char *p = path; *p && *q; p++) {
        if (to_lower((u8)*p) != to_lower((u8)*q)) {
            prev_matched = 0;
            continue;
        }
        score += 1;
        if (prev_matched) score += 8;
        if (is_word_start(path, p)) score += 6;
        if (p >= name) score += 2;
        prev_matched = 1;
        q++;
    }
    if (*q) return -1;
    return score * 256 + (255 - (s32)MIN(str_len(path), 255)); // shorter paths break ties
}

static int picker_compare(const void *a, const void *b)
{
    const PickerMatch *x = a, *y = b;
    if (x->score != y->score) return x->score > y->score ? -1 : 1;
    return x->index < y->index ? -1 : 1;
}

static void picker_filter(EditorState *s)
{
    u32 n = 0;
    for (u32 i = 0; i < s->picker_file_count; i++) {
        s32 score = s->picker_query[0] ? fuzzy_score(s->picker_query, s->picker_files[i]) : 0;
        if (score < 0) continue;
        s->picker_matches[n].score = score;
        s->picker_matches[n].index = i;
        n++;
    }
    if (s->picker_query[0]) qsort(s->picker_matches, n, sizeof(PickerMatch), picker_compare);
    s->picker_match_count = n;
    s->picker_selected = 0;
    s->picker_top = 0;
}

static void picker_open(EditorState *s)
{
    // Root: the workspace folder, else the current file's folder, else the working directory.
    char root[EDITOR_PATH_MAX] = ".";
    if (s->folder[0]) {
        str_copy(root, sizeof(root), s->folder);
    } else if (s->path[0] && path_name(s->path) != s->path) {
        str_copy(root, sizeof(root), s->path);
        root[path_name(s->path) - s->path] = 0;
    }
    // Drop trailing separators, except the one in a drive root like "C:\".
    u64 root_len = str_len(root);
    while (root_len > 1 && is_separator(root[root_len - 1]) && !(root_len == 3 && root[1] == ':')) {
        root[--root_len] = 0;
    }

    arena_reset(&s->picker_arena);
    s->picker_files = arena_push(&s->picker_arena, PICKER_MAX_FILES * sizeof(char *));
    s->picker_matches = arena_push(&s->picker_arena, PICKER_MAX_FILES * sizeof(PickerMatch));
    s->picker_file_count = 0;
    s->picker_truncated = 0;
    if (!s->picker_files || !s->picker_matches) {
        set_status(s, "Out of memory", 0);
        return;
    }

    PickerScan scan;
    scan.s = s;
    scan.depth = 0;
    str_copy(scan.path, sizeof(scan.path), root);
    scan.relative_start = root_len + (is_separator(root[root_len - 1]) ? 0 : 1);
    if (!s->platform->list_directory(root, picker_on_entry, &scan)) {
        set_status(s, "Could not list folder ", root);
        return;
    }

    str_copy(s->picker_root, sizeof(s->picker_root), root);
    s->picker_query[0] = 0;
    picker_filter(s);
    s->picker_open = 1;
    if (s->picker_truncated) set_status(s, "Only the first 20000 files are listed", 0);
}

static void picker_accept(EditorState *s)
{
    if (s->picker_selected >= s->picker_match_count) return;
    const char *relative = s->picker_files[s->picker_matches[s->picker_selected].index];
    char path[EDITOR_PATH_MAX];
    str_copy(path, sizeof(path), s->picker_root);
    if (!is_separator(path[str_len(path) - 1])) str_append(path, sizeof(path), "/");
    str_append(path, sizeof(path), relative);

    s->picker_open = 0;
    if (confirm_discard(s)) load_file(s, path, 0);
}

static int picker_visible_rows(EditorState *s)
{
    int rows = (s->text_bottom - 4 * s->pad) / s->font.line_h - 2;
    return MAX(1, MIN(PICKER_MAX_ROWS, rows));
}

static void picker_move(EditorState *s, int delta)
{
    if (!s->picker_match_count) return;
    s64 selected = (s64)s->picker_selected + delta;
    selected = MAX(0, MIN(selected, (s64)s->picker_match_count - 1));
    s->picker_selected = (u32)selected;

    u32 rows = (u32)picker_visible_rows(s);
    if (s->picker_selected < s->picker_top) s->picker_top = s->picker_selected;
    else if (s->picker_selected >= s->picker_top + rows) s->picker_top = s->picker_selected - rows + 1;
}

static void picker_type(EditorState *s, u32 codepoint)
{
    u64 n = str_len(s->picker_query);
    if (codepoint < 32 || codepoint > 126 || n + 1 >= sizeof(s->picker_query)) return;
    s->picker_query[n] = (char)codepoint;
    s->picker_query[n + 1] = 0;
    picker_filter(s);
}

// While the picker is open it owns every key without Ctrl/Alt; chords still reach the keymap.
static b32 picker_handle_key(EditorState *s, Input *input)
{
    if (input->mods & (MOD_CTRL | MOD_ALT)) return 0;
    int rows = picker_visible_rows(s);
    switch (input->key) {
        case KEY_ESCAPE:    s->picker_open = 0; break;
        case KEY_ENTER:     picker_accept(s); break;
        case KEY_UP:        picker_move(s, -1); break;
        case KEY_DOWN:      picker_move(s, +1); break;
        case KEY_PAGE_UP:   picker_move(s, -rows); break;
        case KEY_PAGE_DOWN: picker_move(s, rows); break;
        case KEY_BACKSPACE: {
            u64 n = str_len(s->picker_query);
            if (n) {
                s->picker_query[n - 1] = 0;
                picker_filter(s);
            }
            break;
        }
        default: break;
    }
    return 1;
}

static void render_picker(EditorState *s, Bitmap *bm)
{
    Font *f = &s->font;
    Rect full = { 0, 0, bm->width, bm->height };
    u32 rows = (u32)picker_visible_rows(s);
    u32 shown = s->picker_match_count > s->picker_top ? MIN(rows, s->picker_match_count - s->picker_top) : 0;
    int inner = s->pad;
    int w = MIN(bm->width - 4 * s->pad, 100 * f->cell_w);
    int x0 = (bm->width - w) / 2;
    int y0 = 2 * s->pad;
    int x1 = x0 + w;
    int y1 = y0 + 3 * inner + (int)(1 + MAX(shown, 1)) * f->line_h;
    Rect inside = { x0 + inner, y0, x1 - inner, y1 };

    draw_rect(bm, full, x0 - 1, y0 - 1, x1 + 1, y1 + 1, THEME_PICKER_EDGE);
    draw_rect(bm, full, x0, y0, x1, y1, THEME_PICKER_BG);

    // Query line, with "matches/files" on the right.
    int y = y0 + inner;
    int baseline = y + f->ascent;
    char count[48] = "";
    str_append_u64(count, sizeof(count), s->picker_match_count);
    str_append(count, sizeof(count), "/");
    str_append_u64(count, sizeof(count), s->picker_file_count);
    int count_x = x1 - inner - (int)str_len(count) * f->cell_w;
    draw_string(s, bm, inside, count_x, baseline, count, THEME_GUTTER_TEXT);

    Rect query_clip = { x0 + inner, y0, count_x - f->cell_w, y1 };
    draw_string(s, bm, query_clip, x0 + inner, baseline, ">", THEME_GUTTER_TEXT);
    int query_x = x0 + inner + 2 * f->cell_w;
    draw_string(s, bm, query_clip, query_x, baseline, s->picker_query, THEME_TEXT);
    int cursor_x = query_x + (int)str_len(s->picker_query) * f->cell_w;
    draw_rect(bm, query_clip, cursor_x, y, cursor_x + MAX(2, dpi_scale(s, 2)), y + f->line_h, THEME_CURSOR);

    y += f->line_h + inner;
    draw_rect(bm, full, x0, y, x1, y + 1, THEME_PICKER_EDGE);
    y += inner;

    if (shown == 0) {
        const char *empty = s->picker_file_count ? "No matching files" : "No files in this folder";
        draw_string(s, bm, inside, x0 + inner, y + f->ascent, empty, THEME_GUTTER_TEXT);
    }
    for (u32 r = 0; r < shown; r++) {
        u32 i = s->picker_top + r;
        int row_y = y + (int)r * f->line_h;
        if (i == s->picker_selected) draw_rect(bm, full, x0, row_y, x1, row_y + f->line_h, THEME_PICKER_SEL);
        const char *file = s->picker_files[s->picker_matches[i].index];
        draw_string(s, bm, inside, x0 + inner, row_y + f->ascent, file, THEME_TEXT);
    }
}

//
// Commands
//

static void save_command(EditorState *s, b32 save_as)
{
    if (!save(s, save_as)) return;
    // Saving the keybinds file applies it right away.
    if (s->config_path[0] && path_equal(s->path, s->config_path)) {
        s->status[0] = 0;
        keymap_load(s); // reports errors in the status bar
        if (!s->status[0]) set_status(s, "Saved, keybinds reloaded", 0);
    }
}

static b32 is_motion(Command command)
{
    return command >= CMD_MOVE_LEFT && command <= CMD_PAGE_DOWN;
}

// extend: Shift is held, so a motion grows the selection instead of dropping it.
static void run_command(EditorState *s, Command command, b32 extend)
{
    GapBuffer *b = &s->text;
    u64 len = gb_length(b);
    int page = MAX(1, s->visible_lines - 1);
    u64 anchor = s->anchor;

    // Left/Right without Shift first collapse a selection to its start/end.
    if (!extend && has_selection(s) && (command == CMD_MOVE_LEFT || command == CMD_MOVE_RIGHT)) {
        u64 start, end;
        selection_range(s, &start, &end);
        move_cursor(s, command == CMD_MOVE_LEFT ? start : end);
        return;
    }

    switch (command) {
        case CMD_MOVE_LEFT:  if (s->cursor > 0) move_cursor(s, s->cursor - 1); break;
        case CMD_MOVE_RIGHT: if (s->cursor < len) move_cursor(s, s->cursor + 1); break;
        case CMD_MOVE_UP:    move_vertical(s, -1); break;
        case CMD_MOVE_DOWN:  move_vertical(s, +1); break;
        case CMD_LINE_START: move_cursor(s, line_start(b, s->cursor)); break;
        case CMD_LINE_END:   move_cursor(s, line_end(b, s->cursor)); break;
        case CMD_DOC_START:  move_cursor(s, 0); break;
        case CMD_DOC_END:    move_cursor(s, len); break;
        case CMD_PAGE_UP:
            for (int i = 0; i < page; i++) move_vertical(s, -1);
            s->top_line -= MIN(s->top_line, (u64)page);
            break;
        case CMD_PAGE_DOWN:
            for (int i = 0; i < page; i++) move_vertical(s, +1);
            s->top_line += (u64)page;
            break;
        case CMD_BACKSPACE:  backspace(s); break;
        case CMD_DELETE:     delete_forward(s); break;
        case CMD_NEWLINE:    newline(s); break;
        case CMD_INSERT_TAB: edit_insert(s, (const u8 *)"    ", 4); break;
        case CMD_SELECT_ALL:       select_range(s, 0, len); break;
        case CMD_SELECT_LINE:      select_line(s); break;
        case CMD_SELECT_PARAGRAPH: select_paragraph(s); break;
        case CMD_CLEAR_SELECTION:  s->anchor = s->cursor; break;
        case CMD_COPY:             copy_to_clipboard(s, 0); break;
        case CMD_CUT:              copy_to_clipboard(s, 1); break;
        case CMD_PASTE:            paste(s); break;
        case CMD_UNDO:       undo(s); break;
        case CMD_REDO:       redo(s); break;
        case CMD_NEW:        if (confirm_discard(s)) doc_reset(s); break;
        case CMD_OPEN:       open_with_dialog(s); break;
        case CMD_SAVE:       save_command(s, 0); break;
        case CMD_SAVE_AS:    save_command(s, 1); break;
        case CMD_QUIT:       if (confirm_discard(s)) s->quit_requested = 1; break;
        case CMD_OPEN_KEYBINDS:
            if (!s->config_path[0]) set_status(s, "No keybinds file on this system", 0);
            else if (confirm_discard(s)) load_file(s, s->config_path, 1);
            break;
        case CMD_RELOAD_KEYBINDS:
            s->status[0] = 0;
            keymap_load(s);
            if (!s->status[0]) set_status(s, "Keybinds reloaded", 0);
            break;
        case CMD_FILE_PICKER: picker_open(s); break;
        default: break;
    }

    if (extend && is_motion(command)) s->anchor = anchor;
}

//
// Entry points
//

static EditorState *editor_setup(Memory *memory, Platform *platform)
{
    u8 *base = memory->base;
    Arena permanent = arena_make(base, PERMANENT_SIZE, platform->commit);
    EditorState *s = arena_push(&permanent, sizeof(EditorState));
    if (!s) return 0;
    memset(s, 0, sizeof(*s));
    s->platform = platform;

    base += PERMANENT_SIZE;
    s->permanent = permanent;
    s->font_arena = arena_make(base, FONT_SIZE, platform->commit);
    base += FONT_SIZE;
    s->document = arena_make(base, DOCUMENT_SIZE, platform->commit);
    base += DOCUMENT_SIZE;
    s->undo_records = arena_make(base, UNDO_RECORDS_SIZE, platform->commit);
    base += UNDO_RECORDS_SIZE;
    s->undo_text = arena_make(base, UNDO_TEXT_SIZE, platform->commit);
    base += UNDO_TEXT_SIZE;
    s->transient = arena_make(base, TRANSIENT_SIZE, platform->commit);
    base += TRANSIENT_SIZE;
    s->picker_arena = arena_make(base, PICKER_SIZE, platform->commit);

    s->text.arena = &s->document;
    s->text.data = s->document.base;
    s->edits = (Edit *)s->undo_records.base;
    s->dpi = 96;
    doc_reset(s);

    memory->initialized = 1;
    return s;
}

static void editor_init(EditorState *s, Input *input)
{
    Platform *p = s->platform;
    s->dpi = input->dpi > 0 ? input->dpi : 96;

    s64 font_size = p->file_size(EDITOR_FONT_PATH);
    if (font_size > 0) {
        u8 *data = arena_push(&s->permanent, (u64)font_size);
        if (data && p->read_file(EDITOR_FONT_PATH, data, (u64)font_size)) s->font_file = data;
    }
    font_bake(s);

    if (input->config_path) str_copy(s->config_path, sizeof(s->config_path), input->config_path);
    keymap_load(s);

    if (input->folder) str_copy(s->folder, sizeof(s->folder), input->folder);
    if (input->path && input->path[0]) load_file(s, input->path, 1);
    if (!s->font.loaded) set_status(s, "Could not load font ", EDITOR_FONT_PATH);
}

Output editor_update_and_render(Memory *memory, Platform *platform, Input *input, Bitmap *bitmap)
{
    Output out = {0};
    if (!memory->initialized) {
        if (input->kind != INPUT_INIT) return out;
        if (!editor_setup(memory, platform)) {
            out.quit = 1;
            return out;
        }
    }

    EditorState *s = (EditorState *)memory->base;
    s->platform = platform;
    arena_reset(&s->transient);

    if (input->kind == INPUT_INIT) editor_init(s, input);
    compute_layout(s, bitmap);

    b32 follow_cursor = 0;
    switch (input->kind) {
        case INPUT_CHAR:
            s->status[0] = 0;
            if (s->picker_open) {
                picker_type(s, input->codepoint);
            } else if (input->codepoint >= 32 && input->codepoint <= 126) {
                type_char(s, (u8)input->codepoint);
            }
            follow_cursor = 1;
            break;
        case INPUT_KEY:
            s->status[0] = 0;
            if (s->picker_open && picker_handle_key(s, input)) {
                out.handled = 1;
            } else {
                Command command = keymap_lookup(s, input->key, input->mods);
                out.handled = command != CMD_NONE;
                run_command(s, command, (input->mods & MOD_SHIFT) != 0);
            }
            follow_cursor = 1;
            if (s->quit_requested) {
                out.quit = 1;
                return out;
            }
            break;
        case INPUT_WHEEL:
            if (input->wheel_lines > 0) s->top_line -= MIN(s->top_line, (u64)input->wheel_lines);
            else s->top_line += (u64)-input->wheel_lines;
            break;
        case INPUT_DPI_CHANGED:
            s->dpi = input->dpi;
            font_bake(s);
            follow_cursor = 1;
            break;
        case INPUT_RESIZE:
            follow_cursor = 1;
            break;
        case INPUT_CLOSE_REQUEST:
            if (confirm_discard(s)) {
                out.quit = 1;
                return out;
            }
            break;
        default:
            break;
    }

    compute_layout(s, bitmap);
    if (follow_cursor) scroll_to_cursor(s);
    if (s->top_line >= s->total_lines) s->top_line = s->total_lines - 1;
    update_title(s);

    if (bitmap->pixels && bitmap->width > 0 && bitmap->height > 0) {
        render(s, bitmap);
        if (s->picker_open) render_picker(s, bitmap);
    }
    out.redraw = 1;
    return out;
}

#undef malloc
#undef realloc
#undef free
