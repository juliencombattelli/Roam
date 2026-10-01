#define main roam_program_main
#include "../roam.c"
#undef main

#include <assert.h>

int main(void)
{
    Directory empty = {0};
    assert(content_width(&empty, 80) == 9);
    unsigned char routes[100] = {0};
    route_visible(routes, 10, 10, 3, -5, 3, 15);
    assert(routes[3 * 10] & LINE_RIGHT);
    assert(routes[3 * 10 + 9] & LINE_LEFT);
    route_visible(routes, 10, 10, -5, 5, 15, 5);
    assert(routes[2 * 10 + 5] & LINE_DOWN);
    assert(routes[6 * 10 + 5] & LINE_UP);
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
    toggle_open(strdup(child));
    Column columns[4] = {{.directory = &root}};
    size_t count = 1;
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