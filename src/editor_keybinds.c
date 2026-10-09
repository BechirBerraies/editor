// editor_keybinds.c - everything about key bindings: the command list, the built-in keybinds.txt,
// its parser, and looking up which command a key chord runs.
// Part of the unity build: included by editor.c after EditorState and the string helpers.
// (Turning OS key codes into Key values is the platform's job, see win32_map_key.)

typedef enum Command {
    CMD_NONE,
    // Motions, kept contiguous: with Shift held they extend the selection (see is_motion).
    CMD_MOVE_LEFT, CMD_MOVE_RIGHT, CMD_MOVE_UP, CMD_MOVE_DOWN,
    CMD_LINE_START, CMD_LINE_END, CMD_DOC_START, CMD_DOC_END, CMD_PAGE_UP, CMD_PAGE_DOWN,
    CMD_BACKSPACE, CMD_DELETE, CMD_NEWLINE, CMD_INSERT_TAB,
    CMD_SELECT_ALL, CMD_SELECT_LINE, CMD_SELECT_PARAGRAPH, CMD_CLEAR_SELECTION,
    CMD_COPY, CMD_CUT, CMD_PASTE,
    CMD_UNDO, CMD_REDO,
    CMD_NEW, CMD_OPEN, CMD_SAVE, CMD_SAVE_AS, CMD_QUIT,
    CMD_OPEN_KEYBINDS, CMD_RELOAD_KEYBINDS, CMD_FILE_PICKER,
    CMD_COUNT
} Command;

// The names used in keybinds.txt.
static const char *command_names[CMD_COUNT] = {
    [CMD_NONE] = "none",
    [CMD_MOVE_LEFT] = "move_left",       [CMD_MOVE_RIGHT] = "move_right",
    [CMD_MOVE_UP] = "move_up",           [CMD_MOVE_DOWN] = "move_down",
    [CMD_LINE_START] = "line_start",     [CMD_LINE_END] = "line_end",
    [CMD_DOC_START] = "doc_start",       [CMD_DOC_END] = "doc_end",
    [CMD_PAGE_UP] = "page_up",           [CMD_PAGE_DOWN] = "page_down",
    [CMD_BACKSPACE] = "backspace",       [CMD_DELETE] = "delete",
    [CMD_NEWLINE] = "newline",           [CMD_INSERT_TAB] = "insert_tab",
    [CMD_SELECT_ALL] = "select_all",     [CMD_SELECT_LINE] = "select_line",
    [CMD_SELECT_PARAGRAPH] = "select_paragraph", [CMD_CLEAR_SELECTION] = "clear_selection",
    [CMD_COPY] = "copy",                 [CMD_CUT] = "cut",
    [CMD_PASTE] = "paste",
    [CMD_UNDO] = "undo",                 [CMD_REDO] = "redo",
    [CMD_NEW] = "new",                   [CMD_OPEN] = "open",
    [CMD_SAVE] = "save",                 [CMD_SAVE_AS] = "save_as",
    [CMD_QUIT] = "quit",
    [CMD_OPEN_KEYBINDS] = "open_keybinds", [CMD_RELOAD_KEYBINDS] = "reload_keybinds",
    [CMD_FILE_PICKER] = "file_picker",
};

// Parsed first, then the user's file is applied on top. Also written out as the starter keybinds.txt.
static const char default_keybinds[] =
    "# Editor keybinds. One binding per line: <chord> <command>\n"
    "# Changes apply when you save this file (Ctrl+S).\n"
    "#\n"
    "# Chord: modifiers joined with '+', then a key, e.g. ctrl+shift+s\n"
    "#   modifiers: ctrl shift alt\n"
    "#   keys: a-z 0-9 f1-f12 left right up down home end pageup pagedown\n"
    "#         backspace delete enter tab escape space comma period minus equals\n"
    "#   Letters, digits and punctuation need ctrl or alt (plain typing inserts text).\n"
    "#   A shifted chord with no binding falls back to the unshifted one, and Shift on a\n"
    "#   movement command extends the selection (shift+right, ctrl+shift+end, ...).\n"
    "#\n"
    "# Commands: move_left move_right move_up move_down line_start line_end\n"
    "#   doc_start doc_end page_up page_down backspace delete newline insert_tab\n"
    "#   select_all select_line select_paragraph clear_selection copy cut paste\n"
    "#   undo redo new open save save_as quit open_keybinds reload_keybinds file_picker\n"
    "#   none (removes a binding)\n"
    "#\n"
    "# Lines you delete here keep their built-in default; bind a chord to none to remove it.\n"
    "\n"
    "left          move_left\n"
    "right         move_right\n"
    "up            move_up\n"
    "down          move_down\n"
    "home          line_start\n"
    "end           line_end\n"
    "ctrl+home     doc_start\n"
    "ctrl+end      doc_end\n"
    "pageup        page_up\n"
    "pagedown      page_down\n"
    "backspace     backspace\n"
    "delete        delete\n"
    "enter         newline\n"
    "tab           insert_tab\n"
    "\n"
    "ctrl+a        select_all\n"
    "ctrl+l        select_line\n"
    "ctrl+shift+l  select_paragraph\n"
    "escape        clear_selection\n"
    "ctrl+c        copy\n"
    "ctrl+x        cut\n"
    "ctrl+v        paste\n"
    "\n"
    "ctrl+z        undo\n"
    "ctrl+y        redo\n"
    "ctrl+shift+z  redo\n"
    "\n"
    "ctrl+n        new\n"
    "ctrl+o        open\n"
    "ctrl+s        save\n"
    "ctrl+shift+s  save_as\n"
    "ctrl+comma    open_keybinds\n"
    "ctrl+p        file_picker\n";

static const struct { const char *name; Key key; } named_keys[] = {
    { "left", KEY_LEFT }, { "right", KEY_RIGHT }, { "up", KEY_UP }, { "down", KEY_DOWN },
    { "home", KEY_HOME }, { "end", KEY_END }, { "pageup", KEY_PAGE_UP }, { "pagedown", KEY_PAGE_DOWN },
    { "backspace", KEY_BACKSPACE }, { "delete", KEY_DELETE }, { "enter", KEY_ENTER }, { "tab", KEY_TAB },
    { "escape", KEY_ESCAPE }, { "space", KEY_SPACE }, { "comma", KEY_COMMA }, { "period", KEY_PERIOD },
    { "minus", KEY_MINUS }, { "equals", KEY_EQUALS },
};

static Key key_from_name(const u8 *text, u64 len)
{
    if (len == 1) {
        u8 c = to_lower(text[0]);
        if (c >= 'a' && c <= 'z') return (Key)(KEY_A + (c - 'a'));
        if (c >= '0' && c <= '9') return (Key)(KEY_0 + (c - '0'));
    }
    if ((len == 2 || len == 3) && to_lower(text[0]) == 'f') {
        int n = 0;
        for (u64 i = 1; i < len; i++) {
            if (text[i] < '0' || text[i] > '9') return KEY_NONE;
            n = n * 10 + (text[i] - '0');
        }
        return n >= 1 && n <= 12 ? (Key)(KEY_F1 + n - 1) : KEY_NONE;
    }
    for (int i = 0; i < (int)(sizeof(named_keys) / sizeof(named_keys[0])); i++) {
        if (span_is(text, len, named_keys[i].name)) return named_keys[i].key;
    }
    return KEY_NONE;
}

// "ctrl+shift+s" -> mods and key. Returns 0 if any part is unknown.
static b32 parse_chord(const u8 *text, u64 len, u32 *mods, Key *key)
{
    *mods = 0;
    *key = KEY_NONE;
    u64 start = 0;
    for (u64 i = 0; i <= len; i++) {
        if (i < len && text[i] != '+') continue;
        const u8 *part = text + start;
        u64 part_len = i - start;
        start = i + 1;
        if (i == len) {
            *key = key_from_name(part, part_len);
        } else if (span_is(part, part_len, "ctrl")) {
            *mods |= MOD_CTRL;
        } else if (span_is(part, part_len, "shift")) {
            *mods |= MOD_SHIFT;
        } else if (span_is(part, part_len, "alt")) {
            *mods |= MOD_ALT;
        } else {
            return 0;
        }
    }
    return *key != KEY_NONE;
}

static Command command_from_name(const u8 *text, u64 len)
{
    for (int i = 0; i < CMD_COUNT; i++) {
        if (span_is(text, len, command_names[i])) return (Command)i;
    }
    return CMD_COUNT;
}

static void str_append_span(char *dst, u64 cap, const u8 *text, u64 len)
{
    u64 n = str_len(dst);
    for (u64 i = 0; i < len && n + 1 < cap; i++) dst[n++] = (char)text[i];
    if (n < cap) dst[n] = 0;
}

static b32 is_blank(u8 c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

// Applies every valid line of a keybinds file to keymap. Returns the number of bad lines;
// the first one is described in error as "<file_name>:<line>: <problem>".
static int keymap_parse(u8 (*keymap)[KEY_COUNT], const u8 *text, u64 n, const char *file_name,
                        char *error, u64 error_cap)
{
    int errors = 0;
    u64 line_number = 0;
    for (u64 i = 0; i < n;) {
        line_number++;
        u64 end = i;
        while (end < n && text[end] != '\n') end++;

        // Up to two words; '#' starts a comment.
        const u8 *words[2] = { 0, 0 };
        u64 word_lens[2] = { 0, 0 };
        int word_count = 0;
        for (u64 j = i; j < end;) {
            while (j < end && is_blank(text[j])) j++;
            if (j >= end || text[j] == '#') break;
            u64 start = j;
            while (j < end && !is_blank(text[j])) j++;
            if (word_count < 2) {
                words[word_count] = text + start;
                word_lens[word_count] = j - start;
            }
            word_count++;
        }
        i = end + 1;
        if (word_count == 0) continue;

        u32 mods = 0;
        Key key = KEY_NONE;
        Command command = CMD_COUNT;
        const char *problem = 0;
        const u8 *bad = 0;
        u64 bad_len = 0;
        if (word_count != 2) {
            problem = "expected <chord> <command>";
        } else if (!parse_chord(words[0], word_lens[0], &mods, &key)) {
            problem = "unknown key ";
            bad = words[0];
            bad_len = word_lens[0];
        } else if ((command = command_from_name(words[1], word_lens[1])) == CMD_COUNT) {
            problem = "unknown command ";
            bad = words[1];
            bad_len = word_lens[1];
        }

        if (problem) {
            if (errors == 0) {
                str_copy(error, error_cap, file_name);
                str_append(error, error_cap, ":");
                str_append_u64(error, error_cap, line_number);
                str_append(error, error_cap, ": ");
                str_append(error, error_cap, problem);
                if (bad) {
                    str_append(error, error_cap, "\"");
                    str_append_span(error, error_cap, bad, bad_len);
                    str_append(error, error_cap, "\"");
                }
            }
            errors++;
            continue;
        }
        keymap[mods][key] = (u8)command;
    }
    return errors;
}

// Rebuilds the keymap: defaults, then the user's file. A missing file is created with the defaults.
static void keymap_load(EditorState *s)
{
    char error[160] = "";
    memset(s->keymap, 0, sizeof(s->keymap));
    int errors = keymap_parse(s->keymap, (const u8 *)default_keybinds, sizeof(default_keybinds) - 1, "defaults",
                              error, sizeof(error));
    EDITOR_ASSERT(errors == 0);
    if (!s->config_path[0]) return;

    Platform *p = s->platform;
    const char *name = path_name(s->config_path);
    s64 size = p->file_size(s->config_path);
    if (size == -1) {
        void *file = p->begin_write(s->config_path);
        if (file) p->end_write(file, p->write(file, default_keybinds, sizeof(default_keybinds) - 1));
        return;
    }

    u8 *data = size >= 0 && size <= (s64)MEGABYTES(1) ? arena_push(&s->transient, (u64)size + 1) : 0;
    if (!data || !p->read_file(s->config_path, data, (u64)size)) {
        set_status(s, "Could not read ", name);
        return;
    }
    errors = keymap_parse(s->keymap, data, (u64)size, name, error, sizeof(error));
    if (errors) {
        set_status(s, error, 0);
        if (errors > 1) {
            str_append(s->status, sizeof(s->status), " (+");
            str_append_u64(s->status, sizeof(s->status), (u64)(errors - 1));
            str_append(s->status, sizeof(s->status), " more)");
        }
    }
}

// The command bound to a chord, or CMD_NONE. A shifted chord with no binding falls back to the
// unshifted one, so Shift+arrows still move until selection gets its own bindings.
static Command keymap_lookup(EditorState *s, Key key, u32 mods)
{
    if (key <= KEY_NONE || key >= KEY_COUNT) return CMD_NONE;
    mods &= MOD_SHIFT | MOD_CTRL | MOD_ALT;
    Command command = (Command)s->keymap[mods][key];
    if (command == CMD_NONE && (mods & MOD_SHIFT)) command = (Command)s->keymap[mods & ~(u32)MOD_SHIFT][key];
    return command;
}
