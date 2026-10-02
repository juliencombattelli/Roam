#define main roam_program_main
#include "../roam.c"
#undef main

#include <assert.h>

#define TEST_RGB_COMPONENT 17

int main(void)
{
    assert(strcmp(ANSI_BOLD, "1") == 0);
    assert(strcmp(ANSI_ITALIC, "3") == 0);
    assert(strcmp(ANSI_UNDERLINE, "4") == 0);
    assert(strcmp(ANSI_STRIKETHROUGH, "9") == 0);
    assert(strcmp(ANSI_BOLD ";" ANSI_ITALIC ";" ANSI_UNDERLINE, "1;3;4") == 0);
    assert(strcmp(COLOR_STATUS_CURRENT, "48;5;234;38;5;231;1") == 0);
    assert(strcmp(COLOR_PERMISSION_DIRECTORY, "48;5;234;38;5;34;1") == 0);
    const char *rgb_parameters = ANSI_BG_RGB(TEST_RGB_COMPONENT, 20, 255) ";"
                                 ANSI_FG_RGB(10, 128, 200);
    assert(strcmp(rgb_parameters, "48;2;17;20;255;38;2;10;128;200") == 0);
    FILE *color_output = tmpfile();
    assert(color_output);
    int original_output = dup(STDOUT_FILENO);
    assert(original_output >= 0 && dup2(fileno(color_output), STDOUT_FILENO) >= 0);
    term_set_color(rgb_parameters);
    assert(fflush(stdout) == 0);
    assert(fseek(color_output, 0, SEEK_SET) == 0);
    char sequence[64];
    size_t sequence_length = fread(sequence, 1, sizeof(sequence) - 1, color_output);
    sequence[sequence_length] = '\0';
    assert(strcmp(sequence, "\033[0;48;2;17;20;255;38;2;10;128;200m") == 0);
    assert(dup2(original_output, STDOUT_FILENO) >= 0);
    close(original_output);
    fclose(color_output);
    int input[2];
    assert(pipe(input) == 0);
    int original_input = dup(STDIN_FILENO);
    assert(original_input >= 0 && dup2(input[0], STDIN_FILENO) >= 0);
    struct { const char *sequence; int expected; } keys[] = {
        {"\033[32;2u", KEY_SHIFT_SPACE}, {"\033[27;2;32~", KEY_SHIFT_SPACE},
        {"A", 'A'}, {"\033[97:65;2u", 'A'},
        {"?", '?'}, {"\033[47:63;2u", '?'}, {"\033[44:63;2u", '?'},
        {"\033[44;2;63u", '?'}, {"\033[44:63;2;63u", '?'}, {"\033[0;;63u", '?'},
        {"\033[44:60;2;63u", '?'}, {"\033[110;1;110u", 'n'},
        {"\033[44:60;2u", '<'}, {"\033[44;2;60u", '<'},
        {"\033[47;2u", 0}, {"\033[44;2;233u", 0},
        {"\033[44;2:3;63u", 0}, {"\033[47;5u", 0}
    };
    for (size_t index = 0; index < sizeof(keys) / sizeof(*keys); ++index) {
        assert(write(input[1], keys[index].sequence, strlen(keys[index].sequence)) > 0);
        assert(read_key() == keys[index].expected);
    }
    assert(dup2(original_input, STDIN_FILENO) >= 0);
    close(original_input);
    close(input[0]);
    close(input[1]);

        const char *const help_lines[] = {
         "browse", "fold", "H / shift-space hide parents", "all parents",
         "edit", "new file", "new directory", "rename", "delete",
         "c               choose directory in cd mode", "quit"
        };
        assert(key_box_width(help_lines, 10, false, 80) ==
            (int)strlen(help_lines[2]) + 4);
        assert(key_box_width(help_lines, 11, true, 80) ==
            (int)strlen(help_lines[9]) + 4);
        assert(key_box_width(help_lines, 10, false, 24) == 22);

    Directory empty = {0};
    assert(content_width(&empty, 80) == 9);
    char size_text[32];
    format_size(46, size_text, sizeof(size_text));
    assert(strcmp(size_text, "46 B") == 0);
    format_size(1536, size_text, sizeof(size_text));
    assert(strcmp(size_text, "1.5 KiB") == 0);
    unsigned char routes[100] = {0};
    route_visible(routes, 10, 10, 3, -5, 3, 15);
    assert(routes[3 * 10] & LINE_RIGHT);
    assert(routes[3 * 10 + 9] & LINE_LEFT);
    route_visible(routes, 10, 10, -5, 5, 15, 5);
    assert(routes[2 * 10 + 5] & LINE_DOWN);
    assert(routes[6 * 10 + 5] & LINE_UP);
    memset(routes, 0, sizeof(routes));
    route_visible(routes, 10, 10, 6, 4, 6, 9);
    route_visible(routes, 10, 10, 6, 9, 7, 9);
    assert(routes[6 * 10 + 9] == (LINE_LEFT | LINE_DOWN));
    assert(routes[7 * 10 + 9] == LINE_UP);
    memset(routes, 0, sizeof(routes));
    route_visible(routes, 10, 10, 6, 9, 6, 12);
    route_visible(routes, 10, 10, 5, 9, 6, 9);
    assert(routes[6 * 10 + 9] == (LINE_UP | LINE_RIGHT));
    memset(routes, 0, sizeof(routes));
    route_visible(routes, 10, 10, 7, 4, 7, 9);
    route_visible(routes, 10, 10, 7, 9, 10, 9);
    assert(routes[7 * 10 + 9] == (LINE_LEFT | LINE_DOWN));
    struct timespec start = {.tv_sec = 10};
    struct timespec tick = {.tv_sec = 10, .tv_nsec = 100000000};
    assert(scroll_position(20, 80, start, tick) == 45);
    assert(scroll_position(80, 20, start, tick) == 55);
    tick.tv_sec = 11;
    assert(scroll_position(20, 80, start, tick) == 80);

    char template[] = "/tmp/roam-layout-XXXXXX";
    char *path = mkdtemp(template);
    assert(path);
    char *child = child_path(path, "opened");
    assert(child && mkdir(child, 0700) == 0);
    Directory root;
    assert(load_directory(strdup(path), &root) == 0);
    Directory *navigation = malloc(sizeof(*navigation));
    assert(navigation && load_directory(strdup(path), navigation) == 0);
    size_t navigation_depth = 1;
    assert(extend_to_root(&navigation, &navigation_depth));
    assert(navigation_depth >= 3 && strcmp(navigation[0].path, "/") == 0);
    for (size_t index = 0; index < navigation_depth - 1; ++index) {
        Directory *parent = &navigation[index];
        const char *next_path = navigation[index + 1].path;
        char *selected = child_path(parent->path, parent->entries[parent->selected]->d_name);
        assert(selected && strcmp(selected, next_path) == 0);
        free(selected);
    }
    Column *ancestor_columns = calloc(navigation_depth + 1, sizeof(*ancestor_columns));
    assert(ancestor_columns);
    ancestor_columns[0].directory = navigation;
    size_t visible_count = 1;
    collect_columns(ancestor_columns, &visible_count, navigation_depth + 1, 0,
                    navigation, navigation_depth);
    assert(visible_count == navigation_depth);
    assert(strcmp(ancestor_columns[visible_count - 1].directory->path, path) == 0);
    int selected = navigation[navigation_depth - 1].selected;
    size_t view_base = navigation_depth - 2;
    bool all_parents_visible = true;
    focus_view_root(navigation_depth, &view_base, &all_parents_visible);
    assert(view_base == navigation_depth - 1 && !all_parents_visible);
    focus_view_root(navigation_depth, &view_base, &all_parents_visible);
    assert(view_base == navigation_depth - 1);
    toggle_open(strdup(child));
    ancestor_columns[0].directory = &navigation[view_base];
    visible_count = 1;
    collect_columns(ancestor_columns, &visible_count, navigation_depth + 1, 0,
                    &navigation[view_base], navigation_depth - view_base);
    assert(visible_count == 2 && strcmp(ancestor_columns[1].directory->path, child) == 0);
    assert(navigation[navigation_depth - 1].selected == selected);
    toggle_open(strdup(child));
    clear_directory_cache();
    free(ancestor_columns);
    for (size_t index = 0; index < navigation_depth; ++index)
        free_directory(&navigation[index]);
    free(navigation);
    assert(setenv("LS_COLORS", "di=01;34", 1) == 0);
    load_colors();
    assert(strcmp(path_color(root.path, strrchr(root.path, '/') + 1), "01;34") == 0);
    assert(strcmp(entry_color(&root, "opened"), "01;34") == 0);
    Directory focused;
    assert(load_directory(strdup(child), &focused) == 0);
    Column columns[4] = {{.directory = &root}};
    size_t count = 1;
    collect_columns(columns, &count, 4, 0, (Directory[]){root, focused}, 2);
    assert(count == 2 && columns[1].directory->path == focused.path);
    assert(!is_open(child));
    free_directory(&focused);

    Directory *browsing = malloc(sizeof(*browsing));
    assert(browsing && load_directory(strdup(path), browsing) == 0);
    size_t browsing_depth = 1;
    size_t browsing_capacity = 1;
    char browse_message[256] = "";
    size_t browse_base = 0;
    assert(enter_directory(&browsing, &browsing_depth, &browsing_capacity,
                           browse_message, sizeof(browse_message)));
    assert(browsing_depth == 2 && is_open(child));
    leave_directory(browsing, &browsing_depth, &browse_base);
    assert(browsing_depth == 1 && is_open(child));
    toggle_open(strdup(child));
    assert(!is_open(child));
    assert(enter_directory(&browsing, &browsing_depth, &browsing_capacity,
                           browse_message, sizeof(browse_message)));
    assert(browsing_depth == 2 && is_open(child));
    toggle_open(strdup(child));
    leave_directory(browsing, &browsing_depth, &browse_base);
    assert(browsing_depth == 1 && !is_open(child));
    free_directory(browsing);
    free(browsing);

    Directory *starting_path = malloc(sizeof(*starting_path));
    assert(starting_path && load_directory(strdup(child), starting_path) == 0);
    browsing_depth = 1;
    assert(extend_to_root(&starting_path, &browsing_depth));
    browse_base = browsing_depth - 1;
    leave_directory(starting_path, &browsing_depth, &browse_base);
    assert(strcmp(starting_path[browsing_depth - 1].path, path) == 0 && is_open(child));
    assert(browse_base == browsing_depth - 1);
    for (size_t index = 0; index < browsing_depth; ++index)
        free_directory(&starting_path[index]);
    free(starting_path);
    toggle_open(strdup(child));
    assert(!is_open(child));
    toggle_open(strdup(child));
    assert(is_open(child));
    count = 1;
    collect_columns(columns, &count, 4, 0, &root, 1);
    assert(count == 2 && cached_count == 1);
    Directory *snapshot = columns[1].directory;

    char *new_entry = child_path(child, "new-entry");
    FILE *file = fopen(new_entry, "w");
    assert(file);
    assert(fclose(file) == 0);
    count = 1;
    collect_columns(columns, &count, 4, 0, &root, 1);
    assert(count == 2 && columns[1].directory == snapshot && snapshot->count == 0);
    clear_directory_cache();
    count = 1;
    collect_columns(columns, &count, 4, 0, &root, 1);
    assert(count == 2 && columns[1].directory->count == 1);

    remember_selection(columns[1].directory);
    char *earlier_entry = child_path(child, "aardvark");
    file = fopen(earlier_entry, "w");
    assert(file);
    assert(fclose(file) == 0);
    clear_directory_cache();
    count = 1;
    collect_columns(columns, &count, 4, 0, &root, 1);
    assert(count == 2 && columns[1].directory->selected == 1);
    assert(strcmp(columns[1].directory->entries[1]->d_name, "new-entry") == 0);

    char *renamed = child_path(path, "renamed");
    assert(renamed && rename(child, renamed) == 0);
    rename_opened_paths(child, renamed);
    assert(is_open(renamed));
    Directory restored;
    assert(load_directory(strdup(renamed), &restored) == 0);
    assert(strcmp(restored.entries[restored.selected]->d_name, "new-entry") == 0);
    free_directory(&restored);
    clear_directory_cache();
    toggle_open(strdup(renamed));
    free(opened_paths);
    opened_paths = NULL;
    free(focused_entries[0].path);
    free(focused_entries[0].entry);
    free(focused_entries);
    focused_entries = NULL;
    focused_count = 0;
    free_directory(&root);
    char *renamed_entry = child_path(renamed, "new-entry");
    char *renamed_earlier = child_path(renamed, "aardvark");
    assert(renamed_entry && renamed_earlier);
    assert(unlink(renamed_entry) == 0);
    assert(unlink(renamed_earlier) == 0);
    assert(rmdir(renamed) == 0);
    assert(rmdir(path) == 0);
    free(new_entry);
    free(earlier_entry);
    free(renamed_entry);
    free(renamed_earlier);
    free(renamed);
    free(child);
    return 0;
}