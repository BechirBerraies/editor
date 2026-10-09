// editor_tests.c - self-test of the editor core: gap buffer, undo grouping, lines and columns,
// load normalization, selection and clipboard, bracket pairing, keybinds parsing and fuzzy matching.
// Part of the unity build: included after editor.c. Run with `editor.exe --selftest`;
// debug builds also run it at startup.

static int g_test_failures;
static Memory *g_test_memory; // for tests that go through editor_update_and_render

static void test_check(EditorState *s, b32 ok, int line, const char *expr)
{
    if (ok) return;
    char message[256] = "self-test failed: editor_tests.c:";
    str_append_u64(message, sizeof(message), (u64)line);
    str_append(message, sizeof(message), ": ");
    str_append(message, sizeof(message), expr);
    str_append(message, sizeof(message), "\n");
    s->platform->debug_print(message);
    g_test_failures++;
}

#define CHECK(x) test_check(s, (x) ? 1 : 0, __LINE__, #x)

static b32 text_is(EditorState *s, const char *expected)
{
    u64 n = str_len(expected);
    if (gb_length(&s->text) != n) return 0;
    for (u64 i = 0; i < n; i++) {
        if (gb_at(&s->text, i) != (u8)expected[i]) return 0;
    }
    return 1;
}

static void type_text(EditorState *s, const char *text)
{
    for (; *text; text++) edit_insert(s, (const u8 *)text, 1);
}

static void test_undo(EditorState *s)
{
    // Typing merges into one undo step.
    doc_reset(s);
    type_text(s, "hello");
    CHECK(text_is(s, "hello") && s->edit_count == 1);
    undo(s);
    CHECK(text_is(s, "") && s->cursor == 0);
    redo(s);
    CHECK(text_is(s, "hello") && s->cursor == 5);

    // A newline ends the group.
    doc_reset(s);
    type_text(s, "ab\ncd");
    undo(s);
    CHECK(text_is(s, "ab\n"));
    undo(s);
    CHECK(text_is(s, ""));
    redo(s);
    redo(s);
    CHECK(text_is(s, "ab\ncd"));

    // Backspaces merge and undo restores the cursor.
    doc_reset(s);
    type_text(s, "abc");
    backspace(s);
    backspace(s);
    CHECK(text_is(s, "a"));
    undo(s);
    CHECK(text_is(s, "abc") && s->cursor == 3);
    undo(s);
    CHECK(text_is(s, ""));

    // Switching between typing and deleting starts a new group.
    doc_reset(s);
    type_text(s, "abc");
    backspace(s);
    type_text(s, "x");
    CHECK(text_is(s, "abx"));
    undo(s);
    CHECK(text_is(s, "ab"));
    undo(s);
    CHECK(text_is(s, "abc"));
    undo(s);
    CHECK(text_is(s, ""));

    // Moving the cursor starts a new group.
    doc_reset(s);
    type_text(s, "ac");
    move_cursor(s, 1);
    type_text(s, "b");
    CHECK(text_is(s, "abc"));
    undo(s);
    CHECK(text_is(s, "ac") && s->cursor == 1);

    // Delete key merges forward and undo keeps the cursor in place.
    doc_reset(s);
    type_text(s, "abcd");
    move_cursor(s, 1);
    delete_forward(s);
    delete_forward(s);
    CHECK(text_is(s, "ad"));
    undo(s);
    CHECK(text_is(s, "abcd") && s->cursor == 1);

    // A new edit clears redo.
    doc_reset(s);
    type_text(s, "abc");
    undo(s);
    type_text(s, "z");
    redo(s);
    CHECK(text_is(s, "z"));

    // Dirty tracking follows the undo position.
    doc_reset(s);
    CHECK(!is_dirty(s));
    type_text(s, "a");
    CHECK(is_dirty(s));
    mark_saved(s);
    CHECK(!is_dirty(s));
    type_text(s, "b");
    CHECK(is_dirty(s) && s->edit_count == 2);
    undo(s);
    CHECK(!is_dirty(s) && text_is(s, "a"));
    undo(s);
    CHECK(is_dirty(s));
    redo(s);
    CHECK(!is_dirty(s));
    undo(s);
    type_text(s, "q"); // the saved state is now unreachable
    undo(s);
    CHECK(is_dirty(s) && text_is(s, ""));
}

static void test_gap_buffer(EditorState *s)
{
    GapBuffer *b = &s->text;

    // The buffer grows in place past its first 64 KB.
    doc_reset(s);
    u64 big = KILOBYTES(200);
    u8 *data = arena_push(&s->transient, big);
    CHECK(data != 0);
    if (!data) return;
    for (u64 i = 0; i < big; i++) data[i] = (u8)('a' + i % 26);
    edit_insert(s, data, big);
    CHECK(gb_length(b) == big && gb_at(b, big - 1) == data[big - 1]);
    move_cursor(s, big / 2);
    type_text(s, "X");
    CHECK(gb_at(b, big / 2) == 'X' && gb_at(b, big / 2 + 1) == data[big / 2] && gb_at(b, 0) == 'a');
    undo(s);
    CHECK(gb_length(b) == big);
    undo(s);
    CHECK(gb_length(b) == 0);
}

static void test_lines(EditorState *s)
{
    GapBuffer *b = &s->text;

    // Lines, tabs and the desired column.
    doc_reset(s);
    type_text(s, "ab\n\tx\nlong line");
    u64 len = gb_length(b);
    CHECK(line_start(b, 4) == 3 && line_end(b, 3) == 5);
    CHECK(visual_col(b, 3, 4) == 4 && pos_at_col(b, 3, 5) == 5 && pos_at_col(b, 3, 2) == 3);
    CHECK(gb_count_newlines(b, 0, len) == 2 && gb_line_to_pos(b, 2) == 6);
    move_cursor(s, len);
    move_vertical(s, -1);
    CHECK(s->cursor == 5);
    move_vertical(s, -1);
    CHECK(s->cursor == 2);
    move_vertical(s, +1);
    move_vertical(s, +1);
    CHECK(s->cursor == len);
}

static void test_load_normalization(EditorState *s)
{
    // BOM stripped, CRLF -> LF, lone CR kept; LF files untouched.
    u8 crlf_text[] = "\xEF\xBB\xBF" "a\r\nb\rc\r\n";
    b32 crlf, bom;
    u64 n = normalize_loaded_text(crlf_text, sizeof(crlf_text) - 1, &crlf, &bom);
    CHECK(n == 6 && memcmp(crlf_text, "a\nb\rc\n", 6) == 0 && crlf && bom);
    u8 lf_text[] = "a\nb\r\n";
    n = normalize_loaded_text(lf_text, sizeof(lf_text) - 1, &crlf, &bom);
    CHECK(n == 5 && !crlf && !bom);
}

// The self-test must not touch the user's clipboard, so it swaps in this one.
static u8 g_fake_clipboard[256];
static u64 g_fake_clipboard_len;

static b32 fake_set_clipboard(const u8 *text, u64 len)
{
    if (len > sizeof(g_fake_clipboard)) return 0;
    memcpy(g_fake_clipboard, text, len);
    g_fake_clipboard_len = len;
    return 1;
}

static const u8 *fake_get_clipboard(u64 *len)
{
    *len = g_fake_clipboard_len;
    return g_fake_clipboard_len ? g_fake_clipboard : 0;
}

static void test_selection(EditorState *s)
{
    // Typing replaces the selection; undo brings it back in two steps (the typing, then the delete).
    doc_reset(s);
    type_text(s, "hello world");
    select_range(s, 0, 5);
    type_text(s, "X");
    CHECK(text_is(s, "X world") && !has_selection(s));
    undo(s);
    CHECK(text_is(s, " world"));
    undo(s);
    CHECK(text_is(s, "hello world"));

    // Shift+motion extends from the anchor; Backspace deletes the selection, not a character before it.
    doc_reset(s);
    type_text(s, "abc\ndef");
    move_cursor(s, 0);
    run_command(s, CMD_MOVE_RIGHT, 1);
    run_command(s, CMD_MOVE_RIGHT, 1);
    CHECK(s->anchor == 0 && s->cursor == 2);
    run_command(s, CMD_MOVE_DOWN, 1);
    CHECK(s->anchor == 0 && s->cursor == 6);
    run_command(s, CMD_BACKSPACE, 0);
    CHECK(text_is(s, "f") && s->cursor == 0 && !has_selection(s));

    // The same through the real input path: a Shift+Right key event from the platform.
    doc_reset(s);
    type_text(s, "abc");
    move_cursor(s, 0);
    if (!s->font.line_h) font_bake(s); // layout needs metrics; without a font file these are the fallbacks
    keymap_load(s);                    // no config path yet, so this is the built-in defaults
    Bitmap no_bitmap = {0};
    Input shift_right = {0};
    shift_right.kind = INPUT_KEY;
    shift_right.key = KEY_RIGHT;
    shift_right.mods = MOD_SHIFT;
    editor_update_and_render(g_test_memory, s->platform, &shift_right, &no_bitmap);
    Output out = editor_update_and_render(g_test_memory, s->platform, &shift_right, &no_bitmap);
    CHECK(out.handled && s->anchor == 0 && s->cursor == 2);

    // Left/Right without Shift collapse the selection to its edges.
    doc_reset(s);
    type_text(s, "abcdef");
    select_range(s, 4, 1);
    run_command(s, CMD_MOVE_RIGHT, 0);
    CHECK(s->cursor == 4 && !has_selection(s));

    // Select line includes the newline, and grows by a line each time.
    doc_reset(s);
    type_text(s, "ab\ncd\nef");
    move_cursor(s, 4);
    select_line(s);
    CHECK(s->anchor == 3 && s->cursor == 6);
    select_line(s);
    CHECK(s->anchor == 3 && s->cursor == 8);

    // A paragraph is the run of non-blank lines around the cursor.
    doc_reset(s);
    type_text(s, "a\nb\n  \nc\n");
    move_cursor(s, 2);
    select_paragraph(s);
    CHECK(s->anchor == 0 && s->cursor == 4);
    move_cursor(s, 5);
    select_paragraph(s);
    CHECK(s->anchor == 4 && s->cursor == 7);

    // Copy, cut and paste (with the fake clipboard).
    Platform saved = *s->platform;
    s->platform->set_clipboard = fake_set_clipboard;
    s->platform->get_clipboard = fake_get_clipboard;

    doc_reset(s);
    type_text(s, "one two");
    select_range(s, 0, 3);
    copy_to_clipboard(s, 0);
    CHECK(g_fake_clipboard_len == 3 && memcmp(g_fake_clipboard, "one", 3) == 0 && text_is(s, "one two"));
    move_cursor(s, 7);
    paste(s);
    CHECK(text_is(s, "one twoone"));
    undo(s);
    CHECK(text_is(s, "one two"));
    select_range(s, 3, 7);
    copy_to_clipboard(s, 1);
    CHECK(text_is(s, "one") && g_fake_clipboard_len == 4 && memcmp(g_fake_clipboard, " two", 4) == 0);

    // With nothing selected, copy and cut take the whole line.
    doc_reset(s);
    type_text(s, "first\nsecond\n");
    move_cursor(s, 2);
    copy_to_clipboard(s, 1);
    CHECK(text_is(s, "second\n") && g_fake_clipboard_len == 6 && memcmp(g_fake_clipboard, "first\n", 6) == 0);

    *s->platform = saved;
}

// Types like the keyboard does: pairing on, '\n' is the Enter command.
static void type_keys(EditorState *s, const char *text)
{
    for (; *text; text++) {
        if (*text == '\n') newline(s);
        else type_char(s, (u8)*text);
    }
}

static void test_auto_pair(EditorState *s)
{
    // Openers insert their closer before blanks, closers and the end, nowhere else.
    doc_reset(s);
    type_keys(s, "([{");
    CHECK(text_is(s, "([{}])") && s->cursor == 3);
    doc_reset(s);
    type_text(s, "foo");
    move_cursor(s, 0);
    type_keys(s, "(");
    CHECK(text_is(s, "(foo") && s->cursor == 1);

    // A closer steps over the same one; otherwise it is typed. Undo goes a bracket at a time.
    doc_reset(s);
    type_keys(s, "f(x)");
    CHECK(text_is(s, "f(x)") && s->cursor == 4);
    type_keys(s, ")");
    CHECK(text_is(s, "f(x))") && s->cursor == 5);
    doc_reset(s);
    type_keys(s, "foo(bar)");
    undo(s);
    CHECK(text_is(s, "foo()") && s->cursor == 4);
    undo(s);
    CHECK(text_is(s, ""));

    // Quotes pair too, but not after a word character or a backslash, and step over themselves.
    doc_reset(s);
    type_keys(s, "\"a\"");
    CHECK(text_is(s, "\"a\"") && s->cursor == 3);
    doc_reset(s);
    type_keys(s, "it's");
    CHECK(text_is(s, "it's"));
    doc_reset(s);
    type_text(s, "\\");
    type_keys(s, "'");
    CHECK(text_is(s, "\\'"));

    // Backspace in an empty pair deletes both halves, as one undo step.
    doc_reset(s);
    type_keys(s, "x(");
    backspace(s);
    CHECK(text_is(s, "x") && s->cursor == 1);
    undo(s);
    CHECK(text_is(s, "x()") && s->cursor == 2);
    doc_reset(s);
    type_keys(s, "(a");
    backspace(s);
    CHECK(text_is(s, "()"));

    // An opener wraps the selection and keeps it selected; each wrap is one undo step.
    doc_reset(s);
    type_text(s, "foo");
    select_range(s, 0, 3);
    type_keys(s, "(");
    CHECK(text_is(s, "(foo)") && s->anchor == 1 && s->cursor == 4);
    type_keys(s, "[");
    CHECK(text_is(s, "([foo])") && s->anchor == 2 && s->cursor == 5);
    select_range(s, 5, 2);
    type_keys(s, "'");
    CHECK(text_is(s, "(['foo'])") && s->anchor == 6 && s->cursor == 3);
    undo(s);
    CHECK(text_is(s, "([foo])"));
    undo(s);
    undo(s);
    CHECK(text_is(s, "foo"));
    redo(s);
    CHECK(text_is(s, "(foo)"));

    // Enter keeps the indent before the cursor, and opens a block between brackets.
    doc_reset(s);
    type_keys(s, "  a\n");
    CHECK(text_is(s, "  a\n  ") && s->cursor == 6);
    doc_reset(s);
    type_text(s, "    a");
    move_cursor(s, 2);
    type_keys(s, "\n");
    CHECK(text_is(s, "  \n    a") && s->cursor == 5);
    doc_reset(s);
    type_keys(s, "    f {\n");
    CHECK(text_is(s, "    f {\n        \n    }") && s->cursor == 16);
    undo(s);
    CHECK(text_is(s, "    f {}") && s->cursor == 7);
    doc_reset(s);
    type_keys(s, "g(\n");
    CHECK(text_is(s, "g(\n    \n)") && s->cursor == 7);
    doc_reset(s);
    type_keys(s, "\"\n");
    CHECK(text_is(s, "\"\n\""));

    // The same through the real input path.
    doc_reset(s);
    if (!s->font.line_h) font_bake(s);
    Bitmap no_bitmap = {0};
    Input paren = {0};
    paren.kind = INPUT_CHAR;
    paren.codepoint = '(';
    editor_update_and_render(g_test_memory, s->platform, &paren, &no_bitmap);
    CHECK(text_is(s, "()") && s->cursor == 1);
}

static void test_keybinds(EditorState *s)
{
    // Defaults parse cleanly, user lines override, bad lines are counted and described.
    static u8 keymap[8][KEY_COUNT];
    char error[160] = "";
    memset(keymap, 0, sizeof(keymap));
    CHECK(keymap_parse(keymap, (const u8 *)default_keybinds, sizeof(default_keybinds) - 1, "defaults",
                       error, sizeof(error)) == 0);
    CHECK(keymap[MOD_CTRL | MOD_SHIFT][KEY_A + ('S' - 'A')] == CMD_SAVE_AS && keymap[0][KEY_LEFT] == CMD_MOVE_LEFT);
    const char *user =
        "# comment\n"
        "  Ctrl+Shift+P   undo   # trailing comment\r\n"
        "alt+f4 quit\n"
        "ctrl+z none\n"
        "ctrll+p undo\n"
        "ctrl+p nothing\n"
        "ctrl+p\n";
    int errors = keymap_parse(keymap, (const u8 *)user, str_len(user), "keybinds.txt", error, sizeof(error));
    CHECK(errors == 3 && strcmp(error, "keybinds.txt:5: unknown key \"ctrll+p\"") == 0);
    CHECK(keymap[MOD_CTRL | MOD_SHIFT][KEY_A + ('P' - 'A')] == CMD_UNDO);
    CHECK(keymap[MOD_ALT][KEY_F1 + 3] == CMD_QUIT && keymap[MOD_CTRL][KEY_A + ('Z' - 'A')] == CMD_NONE);
    CHECK(keymap[MOD_CTRL][KEY_A + ('P' - 'A')] == CMD_FILE_PICKER); // bad lines don't touch the default
}

static void test_fuzzy_match(EditorState *s)
{
    // Subsequence, case-insensitive; file-name and word-start hits rank higher.
    CHECK(fuzzy_score("wined", "src/win32_editor.c") >= 0);
    CHECK(fuzzy_score("WinEd", "src/win32_editor.c") == fuzzy_score("wined", "src/win32_editor.c"));
    CHECK(fuzzy_score("xyz", "src/win32_editor.c") < 0 && fuzzy_score("ba", "ab") < 0);
    CHECK(fuzzy_score("editor", "src/editor.c") > fuzzy_score("editor", "e/d/i/t/o/r.c"));
    CHECK(fuzzy_score("arena", "src/editor_arena.h") > fuzzy_score("arena", "arena_old/readme.md"));
    CHECK(fuzzy_score("", "anything") >= 0);
}

b32 editor_self_test(Memory *memory, Platform *platform)
{
    EditorState *s = memory->initialized ? (EditorState *)memory->base : editor_setup(memory, platform);
    if (!s) return 0;
    arena_reset(&s->transient);
    g_test_failures = 0;
    g_test_memory = memory;

    test_undo(s);
    test_gap_buffer(s);
    test_lines(s);
    test_load_normalization(s);
    test_selection(s);
    test_auto_pair(s);
    test_keybinds(s);
    test_fuzzy_match(s);

    doc_reset(s);
    s->status[0] = 0;
    return g_test_failures == 0;
}
