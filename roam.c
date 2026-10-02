#define _GNU_SOURCE
#define _XOPEN_SOURCE 700

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <grp.h>
#include <inttypes.h>
#include <limits.h>
#include <locale.h>
#include <pwd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wordexp.h>

#define ANSI_STRINGIFY_INNER(value) #value
#define ANSI_STRINGIFY(value) ANSI_STRINGIFY_INNER(value)
#define ANSI_SGR(parameters) "\033[" parameters "m"
#define ANSI_BG_256(value) "48;5;" ANSI_STRINGIFY(value)
#define ANSI_FG_256(value) "38;5;" ANSI_STRINGIFY(value)
#define ANSI_BG_RGB(red, green, blue) \
    "48;2;" ANSI_STRINGIFY(red) ";" ANSI_STRINGIFY(green) ";" ANSI_STRINGIFY(blue)
#define ANSI_FG_RGB(red, green, blue) \
    "38;2;" ANSI_STRINGIFY(red) ";" ANSI_STRINGIFY(green) ";" ANSI_STRINGIFY(blue)
#define ANSI_PERMISSION(style) "0;" ANSI_BG_256(234) ";" style

enum {
    MIN_TERMINAL_COLUMNS = 12,
    MIN_TERMINAL_ROWS = 7,
    NEW_DIRECTORY_MODE = 0777,
    NEW_FILE_MODE = 0666,
    REMOVE_CONFIRM_MIN_COLUMNS = 16,
    STATUS_BAR_RESERVED_COLUMNS = 9,
    STATUS_SUMMARY_MIN_COLUMNS = 32,
    STATUS_METADATA_WIDE_COLUMNS = 100,
    STATUS_METADATA_MEDIUM_COLUMNS = 72,
    STATUS_METADATA_NARROW_COLUMNS = 48,
    STATUS_METADATA_WIDE_WIDTH = 52,
    STATUS_METADATA_MEDIUM_WIDTH = 30,
    STATUS_METADATA_NARROW_WIDTH = 16,
    STATUS_METADATA_EMPTY_MAX_WIDTH = 20,
    STATUS_METADATA_MIN_WIDTH = 11,
    TREE_ENTRY_INDENT = 2,
    TREE_ENTRY_SUFFIX_WIDTH = 2,
    TREE_LEVEL_GUTTER = 3,
    TREE_BRANCH_GAP = 1,
    SCROLL_SPEED = 250, // cells per second
    SCROLL_FRAME_USEC = 16000,
    KEY_IDLE_TIMEOUT_USEC = 200000,
    KEY_SEQUENCE_TIMEOUT_USEC = 50000
};

static const char *const COLOR_METADATA = "0;" ANSI_BG_256(234) ";39";
static const char *const COLOR_METADATA_MUTED = "0;" ANSI_BG_256(234) ";" ANSI_FG_256(244);
static const char *const COLOR_STATUS_TEXT = ANSI_BG_256(234) ";" ANSI_FG_256(252);
static const char *const COLOR_STATUS_ACCENT = ANSI_BG_256(234) ";" ANSI_FG_256(110);
static const char *const COLOR_STATUS_CURRENT = ANSI_BG_256(234) ";" ANSI_FG_256(231) ";1";
static const char *const COLOR_STATUS_PARENT = ANSI_BG_256(234) ";" ANSI_FG_256(80);
static const char *const COLOR_STATUS_SUMMARY = ANSI_BG_256(234) ";" ANSI_FG_256(180);
static const char *const COLOR_STATUS_HELP = ANSI_BG_256(234) ";" ANSI_FG_256(152);
static const char *const COLOR_TREE = ANSI_FG_256(109);
static const char *const COLOR_PERMISSION_DIRECTORY = ANSI_PERMISSION("1;34");
static const char *const COLOR_PERMISSION_LINK = ANSI_PERMISSION("1;36");
static const char *const COLOR_PERMISSION_OTHER = ANSI_PERMISSION("1;37");
static const char *const COLOR_PERMISSION_MISSING = ANSI_PERMISSION("2");
static const char *const COLOR_PERMISSION_READ = ANSI_PERMISSION("32");
static const char *const COLOR_PERMISSION_WRITE = ANSI_PERMISSION("33");
static const char *const COLOR_PERMISSION_EXECUTE = ANSI_PERMISSION("31");
static const char *const COLOR_PERMISSION_SPECIAL = ANSI_PERMISSION("1;35");
static const char *const EMPTY_DIRECTORY_LABEL = "(empty)";
static const char *const SIZE_UNITS[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
static const char *const TREE_GLYPHS[] = {
    "", "│", "─", "╰", "│", "│", "╭", "├",
    "─", "╯", "─", "┴", "╮", "┤", "┬", "┼"
};
static const char *const KEY_HELP_TEXT[] = {
    "↑↓←→            browse",
    "space           toggle directory folding",
    "H / shift-space hide parents",
    "A               reveal all parents",
    "e / Enter       edit",
    "n               new file",
    "N               new directory",
    "r               rename",
    "d               delete",
    "c               cd",
    "Esc / q         quit"
};

typedef struct {
    char *path;
    struct dirent **entries;
    int count;
    int selected;
} Directory;

typedef struct {
    char *key;
    char *sgr;
} ColorRule;

static struct termios original_terminal;
static bool terminal_active;
static volatile sig_atomic_t stopped;
static volatile sig_atomic_t resized;
static ColorRule *color_rules;
static size_t color_rule_count;
static char *color_storage;
static char **opened_paths;
static size_t opened_count;
static Directory **cached_directories;
static size_t cached_count;
static long viewport_offset;
static bool viewport_initialized;
static unsigned char *painted_cells;
static int painted_columns;
static int painted_rows;
static struct {
    char *path;
    char *entry;
} *focused_entries;
static size_t focused_count;

static void remember_selection(const Directory *directory)
{
    if (!directory->count)
        return;
    char *entry = strdup(directory->entries[directory->selected]->d_name);
    if (!entry)
        return;
    for (size_t index = 0; index < focused_count; ++index)
        if (strcmp(focused_entries[index].path, directory->path) == 0) {
            free(focused_entries[index].entry);
            focused_entries[index].entry = entry;
            return;
        }
    char *path = strdup(directory->path);
    if (!path) {
        free(entry);
        return;
    }
    void *grown = realloc(focused_entries, (focused_count + 1) * sizeof(*focused_entries));
    if (!grown) {
        free(entry);
        free(path);
        return;
    }
    focused_entries = grown;
    focused_entries[focused_count].path = path;
    focused_entries[focused_count++].entry = entry;
}

static bool is_open(const char *path)
{
    for (size_t index = 0; index < opened_count; ++index)
        if (strcmp(opened_paths[index], path) == 0)
            return true;
    return false;
}

static void toggle_open(char *path)
{
    for (size_t index = 0; index < opened_count; ++index) {
        if (strcmp(opened_paths[index], path) == 0) {
            free(opened_paths[index]);
            opened_paths[index] = opened_paths[--opened_count];
            free(path);
            return;
        }
    }
    char **grown = realloc(opened_paths, (opened_count + 1) * sizeof(*opened_paths));
    if (grown) {
        opened_paths = grown;
        opened_paths[opened_count++] = path;
    } else {
        free(path);
    }
}

static void keep_open(const char *path)
{
    if (!is_open(path)) {
        char *saved = strdup(path);
        if (saved)
            toggle_open(saved);
    }
}

static void rename_saved_path(char **path, const char *old_path, const char *new_path)
{
    size_t prefix = strlen(old_path);
    if (strncmp(*path, old_path, prefix) != 0 ||
        ((*path)[prefix] && (*path)[prefix] != '/'))
        return;
    size_t length = strlen(new_path) + strlen(*path + prefix) + 1;
    char *updated = malloc(length);
    if (!updated)
        return;
    snprintf(updated, length, "%s%s", new_path, *path + prefix);
    free(*path);
    *path = updated;
}

static void rename_opened_paths(const char *old_path, const char *new_path)
{
    for (size_t index = 0; index < opened_count; ++index) {
        rename_saved_path(&opened_paths[index], old_path, new_path);
    }
    for (size_t index = 0; index < focused_count; ++index)
        rename_saved_path(&focused_entries[index].path, old_path, new_path);
}

static void term_pop_keyboard_mode(void)
{
    fputs("\033[<u", stdout);
}

static void term_push_keyboard_mode(void)
{
    fputs("\033[>28u", stdout);
}

static void term_show_cursor(void)
{
    fputs("\033[?25h", stdout);
}

static void term_hide_cursor(void)
{
    fputs("\033[?25l", stdout);
}

static void term_reset_style(void)
{
    fputs("\033[0m", stdout);
}

static void term_exit_alternate_buffer(void)
{
    term_pop_keyboard_mode();
    term_reset_style();
    term_show_cursor();
    fputs("\033[?1049l", stdout);
}

static void term_enter_alternate_buffer(void)
{
    fputs("\033[?1049h", stdout);
    term_hide_cursor();
    term_push_keyboard_mode();
}

static void term_move_cursor(int row, int column)
{
    printf("\033[%d;%dH", row + 1, column + 1);
}

static void term_clear_screen(void)
{
    fputs("\033[H\033[2J", stdout);
}

static void term_clear_line(void)
{
    fputs("\033[2K", stdout);
}

static void term_set_color(const char *parameters)
{
    printf(ANSI_SGR("%s"), parameters);
}

static void term_reverse_video(void)
{
    fputs("\033[7m", stdout);
}

static void restore_terminal(void)
{
    if (terminal_active) {
        term_exit_alternate_buffer();
        fflush(stdout);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_terminal);
        terminal_active = false;
    }
}

static void on_signal(int signal_number)
{
    if (signal_number == SIGWINCH)
        resized = 1;
    else
        stopped = 1;
}

static int accept_entry(const struct dirent *entry)
{
    return strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0;
}

static int text_width(const char *source, int limit);

static int load_directory(char *path, Directory *directory)
{
    struct dirent **entries = NULL;
    int count = scandir(path, &entries, accept_entry, alphasort);
    if (count < 0)
        return -1;
    *directory = (Directory){
        .path = path,
        .entries = entries,
        .count = count,
    };
    for (size_t saved = 0; saved < focused_count; ++saved)
        if (strcmp(focused_entries[saved].path, path) == 0)
            for (int index = 0; index < count; ++index)
                if (strcmp(focused_entries[saved].entry, entries[index]->d_name) == 0) {
                    directory->selected = index;
                    return 0;
                }
    return 0;
}

static void free_directory(Directory *directory)
{
    for (int index = 0; index < directory->count; ++index)
        free(directory->entries[index]);
    free(directory->entries);
    free(directory->path);
}

static void clear_directory_cache(void)
{
    for (size_t index = 0; index < cached_count; ++index) {
        free_directory(cached_directories[index]);
        free(cached_directories[index]);
    }
    free(cached_directories);
    cached_directories = NULL;
    cached_count = 0;
}

static char *child_path(const char *parent, const char *name)
{
    size_t length = strlen(parent) + strlen(name) + 2;
    char *path = malloc(length);
    if (path)
        snprintf(path, length, "%s%s%s", parent, strcmp(parent, "/") == 0 ? "" : "/", name);
    return path;
}

static void load_colors(void)
{
    const char *setting = getenv("LS_COLORS");
    if (!setting || !*setting)
        return;
    color_storage = strdup(setting);
    if (!color_storage)
        return;
    size_t capacity = 1;
    for (const char *cursor = setting; *cursor; ++cursor)
        if (*cursor == ':')
            ++capacity;
    color_rules = calloc(capacity, sizeof(*color_rules));
    if (!color_rules)
        return;
    for (char *cursor = color_storage; *cursor;) {
        char *part = cursor;
        char *write = cursor;
        char *separator = NULL;
        while (*cursor && *cursor != ':') {
            if (*cursor == '\\' && (cursor[1] == ':' || cursor[1] == '=' || cursor[1] == '\\')) {
                ++cursor;
                *write++ = *cursor++;
                continue;
            }
            if (*cursor == '=' && !separator)
                separator = write;
            *write++ = *cursor++;
        }
        bool more = *cursor == ':';
        *write = '\0';
        if (more)
            ++cursor;
        if (!separator || !separator[1])
            continue;
        *separator++ = '\0';
        bool valid = true;
        for (const char *cursor = separator; *cursor; ++cursor)
            if ((*cursor < '0' || *cursor > '9') && *cursor != ';')
                valid = false;
        if (valid || (strcmp(part, "ln") == 0 && strcmp(separator, "target") == 0))
            color_rules[color_rule_count++] = (ColorRule){part, separator};
    }
}

static const char *color_rule(const char *key)
{
    const char *color = NULL;
    for (size_t index = 0; index < color_rule_count; ++index)
        if (strcmp(color_rules[index].key, key) == 0)
            color = color_rules[index].sgr;
    return color;
}

static const char *path_color(const char *path, const char *name)
{
    struct stat info;
    const char *color = NULL;
    if (lstat(path, &info) < 0) {
        color = color_rule("mi");
        return color;
    }
    if (S_ISLNK(info.st_mode)) {
        const char *link_color = color_rule("ln");
        if (stat(path, &info) < 0) {
            color = color_rule("or");
            if (!color && link_color && strcmp(link_color, "target") != 0)
                color = link_color;
        } else if (link_color && strcmp(link_color, "target") == 0) {
            color = NULL;
        } else {
            color = link_color;
            return color;
        }
        if (color || !link_color || strcmp(link_color, "target") != 0) {
            return color;
        }
    }
    if (S_ISDIR(info.st_mode)) {
        if ((info.st_mode & S_ISVTX) && (info.st_mode & S_IWOTH))
            color = color_rule("tw");
        else if (info.st_mode & S_IWOTH)
            color = color_rule("ow");
        else if (info.st_mode & S_ISVTX)
            color = color_rule("st");
        if (!color)
            color = color_rule("di");
    } else if (S_ISREG(info.st_mode)) {
        if (info.st_mode & S_ISUID)
            color = color_rule("su");
        else if (info.st_mode & S_ISGID)
            color = color_rule("sg");
        else if (info.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))
            color = color_rule("ex");
        else
            color = color_rule("fi");
        for (size_t index = 0; index < color_rule_count; ++index)
            if (color_rules[index].key[0] == '*' &&
                fnmatch(color_rules[index].key, name, 0) == 0)
                color = color_rules[index].sgr;
    } else if (S_ISFIFO(info.st_mode)) {
        color = color_rule("pi");
    } else if (S_ISSOCK(info.st_mode)) {
        color = color_rule("so");
    } else if (S_ISBLK(info.st_mode)) {
        color = color_rule("bd");
    } else if (S_ISCHR(info.st_mode)) {
        color = color_rule("cd");
    }
    return color;
}

static const char *entry_color(const Directory *directory, const char *name)
{
    char *path = child_path(directory->path, name);
    if (!path)
        return NULL;
    const char *color = path_color(path, name);
    free(path);
    return color;
}

static void clipped_text(const char *source, int skip, int width)
{
    mbstate_t state = {0};
    while (*source && width > 0) {
        wchar_t character;
        size_t bytes = mbrtowc(&character, source, MB_CUR_MAX, &state);
        if (bytes == (size_t)-1 || bytes == (size_t)-2 || bytes == 0) {
            if (skip > 0) {
                --skip;
            } else {
                putchar('?');
                --width;
            }
            ++source;
            memset(&state, 0, sizeof(state));
            continue;
        }
        int cells = wcwidth(character);
        if (cells < 0 || character == 127 || character == 27) {
            cells = 1;
            character = '?';
        }
        if (skip >= cells) {
            skip -= cells;
        } else if (skip > 0) {
            putchar(' ');
            width -= cells - skip;
            skip = 0;
        } else if (cells <= width) {
            if (character == '?')
                putchar('?');
            else
                fwrite(source, 1, bytes, stdout);
            width -= cells;
        } else {
            break;
        }
        source += bytes;
    }
}

static void text(const char *source, int width)
{
    clipped_text(source, 0, width);
}

static int text_width(const char *source, int limit)
{
    mbstate_t state = {0};
    int width = 0;
    while (*source && width < limit) {
        wchar_t character;
        size_t bytes = mbrtowc(&character, source, MB_CUR_MAX, &state);
        if (bytes == (size_t)-1 || bytes == (size_t)-2) {
            bytes = 1;
            memset(&state, 0, sizeof(state));
            width++;
        } else {
            int cells = wcwidth(character);
            width += cells < 0 || character == 127 || character == 27 ? 1 : cells;
        }
        source += bytes;
    }
    return width > limit ? limit : width;
}

static int content_width(const Directory *directory, int limit)
{
    int width = directory->count ? TREE_ENTRY_INDENT + TREE_ENTRY_SUFFIX_WIDTH :
                TREE_ENTRY_INDENT + text_width(EMPTY_DIRECTORY_LABEL, limit);
    for (int entry = 0; entry < directory->count; ++entry) {
        int entry_width = TREE_ENTRY_INDENT + text_width(
            directory->entries[entry]->d_name, limit - TREE_ENTRY_INDENT) + TREE_ENTRY_SUFFIX_WIDTH;
        if (entry_width > width)
            width = entry_width;
    }
    return width < limit ? width : limit;
}

typedef struct {
    Directory *directory;
    size_t parent;
    int parent_entry;
    int level;
    int lane;
    int row_start;
} Column;

typedef struct {
    int width;
    int groups;
    long position;
} Level;

static int group_length(const Column *branch)
{
    return branch->directory->count ? branch->directory->count : 1;
}

static int ideal_start(const Column *branches, size_t index)
{
    const Column *branch = &branches[index];
    return branches[branch->parent].row_start + branch->parent_entry -
           (group_length(branch) - 1) / 2;
}

static void collect_columns(Column *columns, size_t *count, size_t capacity, size_t parent,
                            Directory *directories, size_t depth)
{
    Directory *directory = columns[parent].directory;
    for (int entry = 0; entry < directory->count; ++entry) {
        char *path = child_path(directory->path, directory->entries[entry]->d_name);
        if (!path)
            continue;
        Directory *child = NULL;
        for (size_t index = 0; index < depth; ++index)
            if (strcmp(directories[index].path, path) == 0) {
                child = &directories[index];
                break;
            }
        if ((!child && !is_open(path)) || *count == capacity) {
            free(path);
            continue;
        }
        if (!child)
            for (size_t index = 0; index < cached_count; ++index)
                if (strcmp(cached_directories[index]->path, path) == 0) {
                    child = cached_directories[index];
                    break;
                }
        if (!child) {
            child = malloc(sizeof(*child));
            if (!child || load_directory(path, child) < 0) {
                free(child);
                free(path);
                continue;
            }
            Directory **grown = realloc(cached_directories,
                                        (cached_count + 1) * sizeof(*cached_directories));
            if (!grown) {
                free_directory(child);
                free(child);
                continue;
            }
            cached_directories = grown;
            cached_directories[cached_count++] = child;
        } else {
            free(path);
        }
        size_t index = (*count)++;
        columns[index] = (Column){.directory = child,
                      .parent = parent, .parent_entry = entry,
                      .level = columns[parent].level + 1};
        collect_columns(columns, count, capacity, index, directories, depth);
    }
}

enum { LINE_UP = 1, LINE_RIGHT = 2, LINE_DOWN = 4, LINE_LEFT = 8 };

static void route_line(unsigned char *routes, int columns, int rows,
                       int row, int column, int end_row, int end_column)
{
    if (row < 0 || row >= rows || end_row < 0 || end_row >= rows ||
        column < 0 || column >= columns || end_column < 0 || end_column >= columns)
        return;
    while (row != end_row || column != end_column) {
        int next_row = row + (end_row > row) - (end_row < row);
        int next_column = column + (end_column > column) - (end_column < column);
        unsigned char forward = next_row > row ? LINE_DOWN : next_row < row ? LINE_UP :
                                next_column > column ? LINE_RIGHT : LINE_LEFT;
        unsigned char backward = next_row > row ? LINE_UP : next_row < row ? LINE_DOWN :
                                 next_column > column ? LINE_LEFT : LINE_RIGHT;
        routes[row * columns + column] |= forward;
        routes[next_row * columns + next_column] |= backward;
        row = next_row;
        column = next_column;
    }
}

static void route_visible(unsigned char *routes, int columns, int rows,
                          int row, int column, int end_row, int end_column)
{
    if (row == end_row) {
        if (row < 2 || row >= rows - 2)
            return;
        int first = column < end_column ? column : end_column;
        int last = column > end_column ? column : end_column;
        if (first < 0)
            first = 0;
        if (last >= columns)
            last = columns - 1;
        if (first <= last) {
            route_line(routes, columns, rows, row, first, row, last);
            if (column < 0 || end_column < 0)
                routes[row * columns + first] |= LINE_LEFT;
            if (column >= columns || end_column >= columns)
                routes[row * columns + last] |= LINE_RIGHT;
        }
    } else if (column == end_column && column >= 0 && column < columns) {
        int first = row < end_row ? row : end_row;
        int last = row > end_row ? row : end_row;
        if (first < 2)
            first = 2;
        if (last >= rows - 2)
            last = rows - 3;
        if (first <= last) {
            route_line(routes, columns, rows, first, column, last, column);
            if (row < 2 || end_row < 2)
                routes[first * columns + column] |= LINE_UP;
            if (row >= rows - 2 || end_row >= rows - 2)
                routes[last * columns + column] |= LINE_DOWN;
        }
    }
}

static void draw_routes(const unsigned char *routes, int columns, int rows)
{
    for (int row = 1; row < rows - 2; ++row)
        for (int column = 0; column < columns; ++column)
            if (routes[row * columns + column]) {
                term_move_cursor(row, column);
                fputs(TREE_GLYPHS[routes[row * columns + column]], stdout);
            }
}

static void mark_cells(unsigned char *frame, int columns, int rows,
                       int row, int column, int width)
{
    if (!frame || row < 2 || row >= rows - 2 || width <= 0)
        return;
    int first = column < 0 ? 0 : column;
    int last = column + width > columns ? columns : column + width;
    if (first < last)
        memset(frame + (size_t)row * columns + first, 1, (size_t)(last - first));
}

static void erase_old_cells(const unsigned char *frame, int columns, int rows)
{
    if (!painted_cells || !frame)
        return;
    for (int row = 2; row < rows - 2; ++row)
        for (int column = 0; column < columns;) {
            size_t cell = (size_t)row * columns + column;
            if (!painted_cells[cell] || frame[cell]) {
                ++column;
                continue;
            }
            term_move_cursor(row, column);
            do {
                putchar(' ');
                ++column;
                cell = (size_t)row * columns + column;
            } while (column < columns && painted_cells[cell] && !frame[cell]);
        }
}

static void format_size(off_t size, char *output, size_t capacity)
{
    if (size < 1024) {
        snprintf(output, capacity, "%jd B", (intmax_t)size);
    } else {
        double amount = (double)size;
        int unit = 0;
        while (amount >= 1024 && unit < 6) {
            amount /= 1024;
            ++unit;
        }
        snprintf(output, capacity, "%.1f %s", amount, SIZE_UNITS[unit]);
    }
}

static void format_entry_details(const struct stat *info, char *owner_group,
                                 size_t owner_group_size, char *details, size_t details_size)
{
    char owner_name[256], group_name[256], modified[32];
    struct passwd *owner = getpwuid(info->st_uid);
    if (owner)
        snprintf(owner_name, sizeof(owner_name), "%s", owner->pw_name);
    else
        snprintf(owner_name, sizeof(owner_name), "%ju", (uintmax_t)info->st_uid);
    struct group *group = getgrgid(info->st_gid);
    if (group)
        snprintf(group_name, sizeof(group_name), "%s", group->gr_name);
    else
        snprintf(group_name, sizeof(group_name), "%ju", (uintmax_t)info->st_gid);
    struct tm local;
    if (!localtime_r(&info->st_mtime, &local) ||
        !strftime(modified, sizeof(modified), "%Y-%m-%d %H:%M", &local))
        snprintf(modified, sizeof(modified), "?");
    snprintf(owner_group, owner_group_size, "  %s  %s", owner_name, group_name);
    snprintf(details, details_size, "  %s", modified);
}

static void draw_entry_info(const struct stat *info, int row, int column, int columns)
{
    term_move_cursor(row, column);
    term_set_color(COLOR_METADATA);
    if (!info) {
        text("Metadata unavailable", columns);
        int printed = text_width("Metadata unavailable", columns);
        if (printed < columns)
            printf("%*s", columns - printed, "");
        return;
    }

    char mode[] = "----------";
    mode[0] = S_ISDIR(info->st_mode) ? 'd' : S_ISLNK(info->st_mode) ? 'l' :
              S_ISCHR(info->st_mode) ? 'c' : S_ISBLK(info->st_mode) ? 'b' :
              S_ISFIFO(info->st_mode) ? 'p' : S_ISSOCK(info->st_mode) ? 's' : '-';
    const mode_t bits[] = {S_IRUSR, S_IWUSR, S_IXUSR, S_IRGRP, S_IWGRP, S_IXGRP,
                           S_IROTH, S_IWOTH, S_IXOTH};
    for (int index = 0; index < 9; ++index)
        if (info->st_mode & bits[index])
            mode[index + 1] = "rwx"[index % 3];
    if (info->st_mode & S_ISUID)
        mode[3] = mode[3] == 'x' ? 's' : 'S';
    if (info->st_mode & S_ISGID)
        mode[6] = mode[6] == 'x' ? 's' : 'S';
    if (info->st_mode & S_ISVTX)
        mode[9] = mode[9] == 'x' ? 't' : 'T';

    for (int index = 0; index < 10 && index < columns; ++index) {
        const char *color = index == 0 ?
            (mode[0] == 'd' ? COLOR_PERMISSION_DIRECTORY : mode[0] == 'l' ?
             COLOR_PERMISSION_LINK : COLOR_PERMISSION_OTHER) :
            mode[index] == '-' ? COLOR_PERMISSION_MISSING :
            mode[index] == 'r' ? COLOR_PERMISSION_READ :
            mode[index] == 'w' ? COLOR_PERMISSION_WRITE :
            mode[index] == 'x' ? COLOR_PERMISSION_EXECUTE : COLOR_PERMISSION_SPECIAL;
        term_set_color(color);
        putchar(mode[index]);
    }
    term_set_color(COLOR_METADATA);

    char owner_group[640], details[64];
    format_entry_details(info, owner_group, sizeof(owner_group), details, sizeof(details));
    if (columns > 10) {
        int remaining = columns - 10;
        term_set_color(COLOR_METADATA_MUTED);
        text(owner_group, remaining);
        int printed = text_width(owner_group, remaining);
        remaining -= printed;
        term_set_color(COLOR_METADATA);
        text(details, remaining);
        printed = text_width(details, remaining);
        if (printed < remaining)
            printf("%*s", remaining - printed, "");
    }
}

static void draw_status_bar(const Directory *active, int rows, int columns)
{
    int bar_width = columns - STATUS_BAR_RESERVED_COLUMNS;
    char summary[64];
    char *focused = active->count
        ? child_path(active->path, active->entries[active->selected]->d_name) : NULL;
    struct stat info;
    bool has_info = focused && lstat(focused, &info) == 0;
    if (has_info && S_ISDIR(info.st_mode)) {
        DIR *entries = opendir(focused);
        if (entries) {
            size_t count = 0;
            struct dirent *entry;
            while ((entry = readdir(entries)))
                if (accept_entry(entry))
                    ++count;
            closedir(entries);
            snprintf(summary, sizeof(summary), "%zu %s", count,
                     count == 1 ? "item" : "items");
        } else {
            snprintf(summary, sizeof(summary), "? items");
        }
    } else if (has_info) {
        format_size(info.st_size, summary, sizeof(summary));
    } else if (active->count) {
        snprintf(summary, sizeof(summary), "?");
    } else {
        snprintf(summary, sizeof(summary), "%d %s", active->count,
                 active->count == 1 ? "item" : "items");
    }
    int summary_width = columns < STATUS_SUMMARY_MIN_COLUMNS ? 0 : text_width(summary, bar_width - 2);
    int metadata_width = columns >= STATUS_METADATA_WIDE_COLUMNS ? STATUS_METADATA_WIDE_WIDTH :
                         columns >= STATUS_METADATA_MEDIUM_COLUMNS ? STATUS_METADATA_MEDIUM_WIDTH :
                         columns >= STATUS_METADATA_NARROW_COLUMNS ? STATUS_METADATA_NARROW_WIDTH : 0;
    if (has_info) {
        char owner_group[640], details[64];
        format_entry_details(&info, owner_group, sizeof(owner_group), details, sizeof(details));
        int content_width = 10 + text_width(owner_group, INT_MAX) +
                            text_width(details, INT_MAX);
        if (metadata_width > content_width)
            metadata_width = content_width;
    } else if (!active->count) {
        metadata_width = 0;
    } else if (metadata_width > STATUS_METADATA_EMPTY_MAX_WIDTH) {
        metadata_width = STATUS_METADATA_EMPTY_MAX_WIDTH;
    }
    if (metadata_width > bar_width - summary_width - 5)
        metadata_width = bar_width - summary_width - 5;
    if (metadata_width < STATUS_METADATA_MIN_WIDTH)
        metadata_width = 0;
    int summary_column = bar_width - summary_width - (metadata_width ? metadata_width + 2 : 0);
    int left_limit = summary_column - (summary_width ? 2 : 1);
    term_move_cursor(rows - 2, 0);
    term_set_color(COLOR_STATUS_TEXT);
    putchar(' ');
    term_move_cursor(rows - 2, 1);
    char *path = strdup(focused ? focused : active->path);
    const char *segments[2] = {NULL};
    int count = 0;
    if (path) {
        char *state = NULL;
        for (char *part = strtok_r(path, "/", &state); part;
             part = strtok_r(NULL, "/", &state)) {
            if (count == 2) {
                segments[0] = segments[1];
                --count;
            }
            segments[count++] = part;
        }
    }
    if (!count)
        segments[count++] = focused ? focused : active->path;
    else if (count == 1 && path && path[0] == '/') {
        segments[1] = segments[0];
        segments[0] = "/";
        count = 2;
    }
    int first = 0;
    int breadcrumb_width = 0;
    for (int index = 0; index < count; ++index)
        breadcrumb_width += text_width(segments[index], columns) + (index != 0 ? 3 : 0);
    while (first < count - 1 && breadcrumb_width > left_limit - 2) {
        breadcrumb_width -= text_width(segments[first], columns) + 3;
        ++first;
    }
    int used = 1;
    for (int index = first; index < count && used < left_limit - 1; ++index) {
        if (index != first) {
            if (left_limit - used - 1 < 4)
                break;
            term_set_color(COLOR_STATUS_ACCENT);
            text(" › ", left_limit - used - 1);
            used += 3;
        }
        term_set_color(index == count - 1 ? COLOR_STATUS_CURRENT : COLOR_STATUS_PARENT);
        int width = left_limit - used - 1;
        clipped_text(segments[index], 0, width);
        used += text_width(segments[index], width);
    }
    free(path);
    term_set_color(COLOR_STATUS_TEXT);
    term_move_cursor(rows - 2, used);
    if (used < summary_column)
        printf("%*s", summary_column - used, "");
    term_set_color(COLOR_STATUS_SUMMARY);
    term_move_cursor(rows - 2, summary_column);
    text(summary, summary_width);
    if (metadata_width) {
        term_set_color(COLOR_STATUS_TEXT);
        fputs("  ", stdout);
        if (active->count)
            draw_entry_info(has_info ? &info : NULL, rows - 2,
                            summary_column + summary_width + 2, metadata_width);
        else
            printf("%*s", metadata_width, "");
    }
    term_move_cursor(rows - 2, bar_width);
    term_set_color(COLOR_STATUS_HELP);
    fputs("  ? keys ", stdout);
    term_reset_style();
    free(focused);
}

static long draw(const Directory *directories, size_t depth, const char *message,
                 bool preserve_offset, bool redraw_chrome)
{
    struct winsize size;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) < 0 || !size.ws_col || !size.ws_row)
        return viewport_offset;
    int columns = size.ws_col;
    int rows = size.ws_row;
    if (columns < MIN_TERMINAL_COLUMNS || rows < MIN_TERMINAL_ROWS) {
        free(painted_cells);
        painted_cells = NULL;
        term_clear_screen();
        term_move_cursor(0, 0);
        text("Terminal too small", columns);
        fflush(stdout);
        return viewport_offset;
    }

    const Directory *active = &directories[depth - 1];

    size_t capacity = opened_count + depth + 2;
    Column *branches = calloc(capacity, sizeof(*branches));
    if (!branches)
        return viewport_offset;
    branches[0].directory = (Directory *)&directories[0];
    size_t count = 1;
    collect_columns(branches, &count, capacity, 0, (Directory *)directories, depth);
    size_t active_index = 0;
    Level *levels = calloc(count, sizeof(*levels));
    if (!levels) {
        free(branches);
        return viewport_offset;
    }
    for (size_t index = 0; index < count; ++index)
        if (strcmp(branches[index].directory->path, active->path) == 0)
            active_index = index;
    int last_level = 0;
    for (size_t index = 0; index < count; ++index) {
        int level = branches[index].level;
        if (level > last_level)
            last_level = level;
        int width = content_width(branches[index].directory, columns - 2);
        if (width > levels[level].width)
            levels[level].width = width;
        levels[level].groups++;
    }
    size_t *spine = malloc((size_t)(last_level + 1) * sizeof(*spine));
    size_t *group = malloc(count * sizeof(*group));
    if (!spine || !group) {
        free(spine);
        free(group);
        free(levels);
        free(branches);
        return viewport_offset;
    }
    for (int level = 0; level <= last_level; ++level)
        spine[level] = SIZE_MAX;
    size_t ancestor = active_index;
    while (true) {
        spine[branches[ancestor].level] = ancestor;
        if (!ancestor)
            break;
        ancestor = branches[ancestor].parent;
    }
    for (int level = branches[active_index].level + 1; level <= last_level; ++level) {
        size_t parent = spine[level - 1];
        for (size_t index = 1; index < count; ++index)
            if (branches[index].parent == parent &&
                branches[index].parent_entry == branches[parent].directory->selected) {
                spine[level] = index;
                break;
            }
        if (spine[level] == SIZE_MAX)
            break;
    }
    for (int level = 0; level <= last_level; ++level) {
        size_t groups = 0;
        size_t pin = SIZE_MAX;
        for (size_t index = 0; index < count; ++index)
            if (branches[index].level == level) {
                if (index == spine[level])
                    pin = groups;
                group[groups++] = index;
            }
        size_t below = 0;
        int previous_end = INT_MIN / 2;
        if (pin != SIZE_MAX) {
            size_t pinned = group[pin];
            int entry = branches[pinned].directory->selected;
            if (level < last_level && spine[level + 1] != SIZE_MAX &&
                branches[spine[level + 1]].parent == pinned)
                entry = branches[spine[level + 1]].parent_entry;
            branches[pinned].row_start = -entry;
            int next_start = branches[pinned].row_start;
            for (size_t slot = pin; slot > 0; --slot) {
                size_t index = group[slot - 1];
                int ideal = ideal_start(branches, index);
                int limit = next_start - group_length(&branches[index]) - TREE_BRANCH_GAP - 1;
                branches[index].row_start = ideal < limit ? ideal : limit;
                next_start = branches[index].row_start;
            }
            previous_end = branches[pinned].row_start + group_length(&branches[pinned]) - 1;
            below = pin + 1;
        }
        for (size_t slot = below; slot < groups; ++slot) {
            size_t index = group[slot];
            int ideal = level ? ideal_start(branches, index) : 0;
            int limit = previous_end + TREE_BRANCH_GAP + 1;
            branches[index].row_start = ideal > limit ? ideal : limit;
            previous_end = branches[index].row_start + group_length(&branches[index]) - 1;
        }
    }
    free(group);
    free(spine);
    for (int level = 1; level <= last_level; ++level) {
        int raised = 0;
        for (size_t index = 1; index < count; ++index)
            if (branches[index].level == level &&
                branches[index].row_start + group_length(&branches[index]) - 1 <
                    branches[branches[index].parent].row_start + branches[index].parent_entry)
                ++raised;
        int up = 0;
        int down = 0;
        for (size_t index = 1; index < count; ++index) {
            if (branches[index].level != level)
                continue;
            bool above = branches[index].row_start + group_length(&branches[index]) - 1 <
                         branches[branches[index].parent].row_start + branches[index].parent_entry;
            branches[index].lane = above ? raised - 1 - up++ : down++;
        }
    }
    for (int level = 1; level <= last_level; ++level)
        levels[level].position = levels[level - 1].position + levels[level - 1].width +
                                 levels[level].groups + TREE_LEVEL_GUTTER;
    long target_offset = (columns - levels[branches[active_index].level].width) / 2 -
                         levels[branches[active_index].level].position;
    if (!preserve_offset || !viewport_initialized) {
        viewport_offset = target_offset;
        viewport_initialized = true;
    }
    long offset = viewport_offset;
    int center_row = 2 + (rows - 4) / 2;
    const Directory *root = branches[0].directory;
    const char *root_name = strrchr(root->path, '/');
    root_name = root_name && root_name[1] ? root_name + 1 : root->path;
    int root_column = (int)(levels[0].position + offset);
    int root_row = center_row + branches[0].row_start + root->selected;
    int root_width = text_width(root_name, columns);
    bool root_label_visible = root_row >= 2 && root_row < rows - 2 &&
                              root_column > root_width + 2;
    size_t cells = (size_t)rows * columns;
    unsigned char *frame = calloc(cells, 1);
    if (!frame || !painted_cells || painted_columns != columns || painted_rows != rows) {
        term_clear_screen();
        free(painted_cells);
        painted_cells = NULL;
    }
    unsigned char *rails = calloc(cells, 1);
    if (rails) {
        const Column *branch = &branches[0];
        int column = (int)(levels[0].position + offset);
        int top = center_row + branch->row_start;
        int entries = group_length(branch);
        for (int entry = 0; entry < entries; ++entry) {
            int row = top + entry;
            if (row >= 2 && row < rows - 2)
                route_line(rails, columns, rows, row, column, row, column + 1);
        }
        int first = top < 2 ? 2 : top;
        int last = top + entries - 1;
        if (last >= rows - 2)
            last = rows - 3;
        if (first < last)
            route_line(rails, columns, rows, first, column, last, column);
        if (root_label_visible)
            route_line(rails, columns, rows, root_row, root_column - 1,
                       root_row, root_column);
    }
    for (size_t index = 0; index < count; ++index) {
        const Column *branch = &branches[index];
        const Directory *directory = branch->directory;
        const Level *level = &levels[branch->level];
        int width = level->width;
        long position = level->position + offset;
        if (position + width <= 0 || position >= columns) {
            continue;
        }
        int column = (int)position;
        int left = column < 0 ? 0 : column;
        int visible_width = column < 0 ? column + width : width;
        if (visible_width > columns - left)
            visible_width = columns - left;
        int name_column = column + TREE_ENTRY_INDENT;
        int name_left = name_column < 0 ? 0 : name_column;
        int name_skip = name_column < 0 ? -name_column : 0;
        int name_width = visible_width - TREE_ENTRY_INDENT;
        int entry_top_row = center_row + branch->row_start;
        if (!directory->count) {
            if (entry_top_row >= 2 && entry_top_row < rows - 2 && name_width > 0) {
                term_move_cursor(entry_top_row, name_left);
                clipped_text(EMPTY_DIRECTORY_LABEL, name_skip, name_width);
                int label_width = text_width(EMPTY_DIRECTORY_LABEL, name_skip + name_width) - name_skip;
                mark_cells(frame, columns, rows, entry_top_row, name_left, label_width);
            }
        }
        for (int entry = 0; entry < directory->count; ++entry) {
            int row = entry_top_row + entry;
            if (row >= 2 && row < rows - 2 && name_width > 0) {
                const char *color = color_rule_count
                    ? entry_color(directory, directory->entries[entry]->d_name) : NULL;
                if (color)
                    term_set_color(color);
                if (index == active_index && entry == directory->selected)
                    term_reverse_video();
                term_move_cursor(row, name_left);
                clipped_text(directory->entries[entry]->d_name, name_skip, name_width);
                char *path = child_path(directory->path, directory->entries[entry]->d_name);
                struct stat info;
                int printed = text_width(directory->entries[entry]->d_name,
                                         name_skip + name_width) - name_skip;
                if (printed < 0)
                    printed = 0;
                if (path && stat(path, &info) == 0 && S_ISDIR(info.st_mode) &&
                    printed + 2 <= name_width) {
                    fputs(" ›", stdout);
                    printed += 2;
                }
                mark_cells(frame, columns, rows, row, name_left, printed);
                free(path);
                term_reset_style();
            }
        }
    }
    unsigned char *routes = calloc(cells, 1);
    for (size_t index = 1; routes && index < count; ++index) {
        Column *child = &branches[index];
        Column *parent = &branches[child->parent];
        long start = levels[parent->level].position + offset;
        long end = levels[child->level].position + offset;
        int parent_width = levels[parent->level].width;
        if (!parent->directory->count)
            continue;
        int parent_row = center_row + parent->row_start + child->parent_entry;
        int first_row = center_row + child->row_start;
        int last_row = first_row + group_length(child) - 1;
        int name_end = (int)start + TREE_ENTRY_INDENT + text_width(
            parent->directory->entries[child->parent_entry]->d_name,
            parent_width - TREE_ENTRY_INDENT) + TREE_ENTRY_SUFFIX_WIDTH;
        int bar = (int)end - 1 - child->lane;
        int join = parent_row > last_row ? first_row :
                   parent_row < first_row ? last_row : parent_row;
        for (int entry = 0; entry < group_length(child); ++entry)
            route_visible(routes, columns, rows, first_row + entry, bar,
                          first_row + entry, (int)end + 1);
        route_visible(routes, columns, rows, parent_row, name_end, parent_row, bar);
        route_visible(routes, columns, rows, parent_row, bar, join, bar);
        route_visible(routes, columns, rows, first_row, bar, last_row, bar);
    }
    if (rails && routes)
        for (size_t cell = 0; cell < cells; ++cell)
            routes[cell] |= rails[cell];
    if (routes) {
        term_set_color(COLOR_TREE);
        draw_routes(routes, columns, rows);
        term_reset_style();
        if (frame)
            for (size_t cell = 0; cell < cells; ++cell)
                if (routes[cell])
                    frame[cell] = 1;
    }
    if (root_label_visible) {
        term_move_cursor(root_row, root_column - root_width - 2);
        const char *color = color_rule_count ? path_color(root->path, root_name) : NULL;
        if (color)
            term_set_color(color);
        clipped_text(root_name, 0, root_width);
        mark_cells(frame, columns, rows, root_row, root_column - root_width - 2, root_width);
        term_reset_style();
        term_move_cursor(root_row, root_column - 1);
        term_set_color(COLOR_TREE);
        fputs("─", stdout);
        mark_cells(frame, columns, rows, root_row, root_column - 1, 1);
        term_reset_style();
    }
    erase_old_cells(frame, columns, rows);
    free(painted_cells);
    painted_cells = frame;
    painted_columns = columns;
    painted_rows = rows;
    free(rails);
    free(routes);
    free(levels);
    free(branches);
    if (redraw_chrome) {
        draw_status_bar(active, rows, columns);
        term_move_cursor(rows - 1, 0);
        if (*message) {
            term_clear_line();
            text(message, columns);
        } else
            term_clear_line();
    }
    fflush(stdout);
    return target_offset;
}

static long scroll_position(long origin, long target, struct timespec start, struct timespec now)
{
    long double elapsed = (long double)(now.tv_sec - start.tv_sec) +
                          (long double)(now.tv_nsec - start.tv_nsec) / 1000000000;
    long double distance = (long double)target - origin;
    if (distance < 0)
        distance = -distance;
    if (elapsed * SCROLL_SPEED >= distance)
        return target;
    long step = elapsed > 0 ? (long)(elapsed * SCROLL_SPEED) : 0;
    return origin + (target > origin ? step : -step);
}

static void scroll_depth(const Directory *directories, size_t depth,
                         const char *message, long target)
{
    struct timespec start;
    if (viewport_offset == target || clock_gettime(CLOCK_MONOTONIC, &start) < 0)
        return;
    long origin = viewport_offset;
    while (!stopped && !resized && viewport_offset != target) {
        fd_set input;
        FD_ZERO(&input);
        FD_SET(STDIN_FILENO, &input);
        struct timeval timeout = {.tv_usec = SCROLL_FRAME_USEC};
        if (select(STDIN_FILENO + 1, &input, NULL, NULL, &timeout) != 0)
            break;
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
            break;
        long next = scroll_position(origin, target, start, now);
        if (next != viewport_offset) {
            viewport_offset = next;
            draw(directories, depth, message, true, false);
        }
    }
}

enum { KEY_SHIFT_SPACE = 256, KEY_UP, KEY_DOWN, KEY_RIGHT, KEY_LEFT };

static int read_key(void)
{
    static int pending_key;
    if (pending_key) {
        int key = pending_key;
        pending_key = 0;
        return key;
    }
    fd_set input;
    FD_ZERO(&input);
    FD_SET(STDIN_FILENO, &input);
    struct timeval timeout = {.tv_usec = KEY_IDLE_TIMEOUT_USEC};
    int ready = select(STDIN_FILENO + 1, &input, NULL, NULL, &timeout);
    if (ready <= 0)
        return 0;
    unsigned char key;
    if (read(STDIN_FILENO, &key, 1) != 1)
        return -1;
    if (key != 27)
        return key;

    timeout.tv_usec = KEY_SEQUENCE_TIMEOUT_USEC;
    FD_ZERO(&input);
    FD_SET(STDIN_FILENO, &input);
    if (select(STDIN_FILENO + 1, &input, NULL, NULL, &timeout) <= 0)
        return 27;
    unsigned char prefix;
    if (read(STDIN_FILENO, &prefix, 1) != 1)
        return 0;
    if (prefix != '[' && prefix != 'O') {
        pending_key = prefix;
        return 27;
    }
    timeout.tv_usec = KEY_SEQUENCE_TIMEOUT_USEC;
    FD_ZERO(&input);
    FD_SET(STDIN_FILENO, &input);
    if (select(STDIN_FILENO + 1, &input, NULL, NULL, &timeout) <= 0)
        return 0;
    if (read(STDIN_FILENO, &key, 1) != 1)
        return 0;
    if (prefix == '[' && key >= '0' && key <= '9') {
        char sequence[64];
        size_t length = 0;
        sequence[length++] = (char)key;
        while (length < sizeof(sequence) - 1) {
            timeout.tv_usec = KEY_SEQUENCE_TIMEOUT_USEC;
            FD_ZERO(&input);
            FD_SET(STDIN_FILENO, &input);
            if (select(STDIN_FILENO + 1, &input, NULL, NULL, &timeout) <= 0 ||
                read(STDIN_FILENO, &key, 1) != 1)
                return 0;
            sequence[length++] = (char)key;
            if (key >= 0x40 && key <= 0x7e)
                break;
        }
        sequence[length] = '\0';
        if (strcmp(sequence, "27;2;32~") == 0)
            return KEY_SHIFT_SPACE;
        char *end;
        long code = strtol(sequence, &end, 10);
        long shifted = 0;
        if (*end == ':') {
            shifted = strtol(end + 1, &end, 10);
            if (*end == ':')
                strtol(end + 1, &end, 10);
        }
        long modifiers = 1;
        if (*end == ';') {
            ++end;
            if (*end != ';' && *end != 'u')
                modifiers = strtol(end, &end, 10);
        }
        long event = 1;
        if (*end == ':')
            event = strtol(end + 1, &end, 10);
        long produced = 0;
        bool has_text = *end == ';';
        if (has_text) {
            produced = strtol(end + 1, &end, 10);
            if (*end == ':')
                produced = 0;
        }
        if (*end == 'u') {
            if (modifiers < 1 || event == 3)
                return 0;
            long bits = modifiers - 1;
            long keys = bits & ~(64L | 128L);
            if (code == 32 && keys == 1)
                return KEY_SHIFT_SPACE;
            if (code == 99 && keys == 4)
                return 3;
            if (keys == 0 || keys == 1) {
                if (has_text)
                    return produced >= 1 && produced <= 127 ? (int)produced : 0;
                if (keys == 1 && shifted)
                    return shifted >= 1 && shifted <= 127 ? (int)shifted : 0;
            }
            if (code >= 'a' && code <= 'z' && (keys == 0 || keys == 1))
                return (bits & 1) != ((bits & 64) != 0) ? (int)code - 'a' + 'A' : (int)code;
            return keys == 0 && code >= 1 && code <= 127 ? (int)code : 0;
        }
        if (code == 1 && *end >= 'A' && *end <= 'D' && end[1] == '\0')
            return KEY_UP + *end - 'A';
        return 0;
    }
    return key >= 'A' && key <= 'D' ? KEY_UP + key - 'A' : 0;
}

static int key_box_width(const char *const controls[], int count, bool cd_mode, int columns)
{
    int longest = text_width("Esc / q close", columns);
    for (int index = 0; index < count; ++index) {
        int control = index + (!cd_mode && index >= 9);
        int length = text_width(controls[control], columns);
        if (length > longest)
            longest = length;
    }
    return longest + 4 < columns - 2 ? longest + 4 : columns - 2;
}

static void show_keys(bool cd_mode)
{
    int count = cd_mode ? 11 : 10;
    int first = 0;
    bool repaint = true;
    while (!stopped) {
        if (repaint || resized) {
            struct winsize size;
            if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) < 0 ||
                size.ws_col < MIN_TERMINAL_COLUMNS || size.ws_row < MIN_TERMINAL_ROWS)
                break;
            if (resized)
                term_clear_screen();
            int width = key_box_width(KEY_HELP_TEXT, count, cd_mode, size.ws_col);
            int height = size.ws_row - 2 < count + 4 ? size.ws_row - 2 : count + 4;
            int top = (size.ws_row - height) / 2;
            int left = (size.ws_col - width) / 2;
            int visible = height - 4;
            if (first > count - visible)
                first = count - visible;
            term_set_color(COLOR_STATUS_ACCENT);
            term_move_cursor(top, left);
            fputs("╭", stdout);
            for (int column = 0; column < width - 2; ++column)
                fputs("─", stdout);
            fputs("╮", stdout);
            for (int row = 1; row < height - 1; ++row) {
                term_move_cursor(top + row, left);
                fputs("│", stdout);
                term_set_color(COLOR_STATUS_TEXT);
                printf("%*s", width - 2, "");
                term_set_color(COLOR_STATUS_ACCENT);
                fputs("│", stdout);
            }
            term_move_cursor(top + height - 1, left);
            fputs("╰", stdout);
            for (int column = 0; column < width - 2; ++column)
                fputs("─", stdout);
            fputs("╯", stdout);
            term_set_color(COLOR_STATUS_CURRENT);
            term_move_cursor(top + 1, left + 2);
            text("Keys", width - 4);
            term_set_color(COLOR_STATUS_TEXT);
            for (int index = 0; index < visible; ++index) {
                int control = first + index;
                if (!cd_mode && control >= 9)
                    ++control;
                term_move_cursor(top + 2 + index, left + 2);
                text(KEY_HELP_TEXT[control], width - 4);
            }
            term_set_color(COLOR_STATUS_ACCENT);
            term_move_cursor(top + height - 2, left + 2);
            text("Esc / q close", width - 4);
            term_reset_style();
            fflush(stdout);
            repaint = false;
            resized = 0;
        }
        int key = read_key();
        if (key == 27 || key == 'q' || key == -1 || key == 3)
            break;
        struct winsize size;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) < 0)
            continue;
        int visible = (size.ws_row - 2 < count + 4 ? size.ws_row - 2 : count + 4) - 4;
        if (key == KEY_DOWN && first + visible < count) {
            ++first;
            repaint = true;
        } else if (key == KEY_UP && first > 0) {
            --first;
            repaint = true;
        }
    }
}

static bool prompt_name_input(const char *label, const char *initial, char *name, size_t capacity)
{
    struct winsize size;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) < 0 || size.ws_row < MIN_TERMINAL_ROWS)
        return false;
    size_t length = strlen(initial);
    if (length >= capacity)
        return false;
    memcpy(name, initial, length + 1);
    bool dirty = true;
    while (!stopped) {
        if (dirty) {
            term_move_cursor(size.ws_row - 1, 0);
            term_clear_line();
            const char *visible_label = strlen(label) >= size.ws_col ? "Name: " : label;
            int remaining = size.ws_col - (int)strlen(visible_label);
            text(visible_label, size.ws_col);
            int skip = text_width(name, INT_MAX - 4) - remaining;
            clipped_text(name, skip > 0 ? skip : 0, remaining);
            fflush(stdout);
            dirty = false;
        }
        int key = read_key();
        if (key == -1 || key == 27)
            return false;
        if (key == '\r' || key == '\n')
            return length && strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
        if (key == 127 || key == 8) {
            if (length) {
                do {
                    --length;
                } while (length && ((unsigned char)name[length] & 0xc0) == 0x80);
                name[length] = '\0';
                dirty = true;
            }
        } else if (key >= 32 && key < KEY_SHIFT_SPACE && key != 127 &&
                 key != '/' && length + 1 < capacity) {
            name[length++] = (char)key;
            name[length] = '\0';
            dirty = true;
        }
    }
    return false;
}

static bool prompt_name(const char *label, const char *initial, char *name, size_t capacity)
{
    term_pop_keyboard_mode();
    fflush(stdout);
    bool accepted = prompt_name_input(label, initial, name, capacity);
    term_push_keyboard_mode();
    fflush(stdout);
    return accepted;
}

static bool confirm_remove_input(const char *name)
{
    struct winsize size;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) < 0 || size.ws_row < MIN_TERMINAL_ROWS)
        return false;
    term_move_cursor(size.ws_row - 1, 0);
    term_clear_line();
    if (size.ws_col < REMOVE_CONFIRM_MIN_COLUMNS) {
        text("Remove? y/N", size.ws_col);
    } else {
        text("Remove ", 7);
        text(name, size.ws_col - 15);
        text("? [y/N]", 7);
    }
    fflush(stdout);
    int key;
    do {
        key = read_key();
    } while (!stopped && key == 0);
    return key == 'y';
}

static bool confirm_remove(const char *name)
{
    term_pop_keyboard_mode();
    fflush(stdout);
    bool confirmed = confirm_remove_input(name);
    term_push_keyboard_mode();
    fflush(stdout);
    return confirmed;
}

static int refresh_directory(Directory *directory, const char *focus)
{
    char *path = strdup(directory->path);
    if (!path)
        return -1;
    Directory updated;
    if (load_directory(path, &updated) < 0) {
        free(path);
        return -1;
    }
    updated.selected = directory->selected;
    for (int index = 0; focus && index < updated.count; ++index)
        if (strcmp(updated.entries[index]->d_name, focus) == 0) {
            updated.selected = index;
            break;
        }
    if (updated.selected >= updated.count)
        updated.selected = updated.count ? updated.count - 1 : 0;
    free_directory(directory);
    *directory = updated;
    return 0;
}

static int load_parent_directory(const Directory *current, Directory *parent)
{
    char *path = strdup(current->path);
    if (!path)
        return -1;
    char *slash = strrchr(path, '/');
    const char *name = strrchr(current->path, '/') + 1;
    if (slash == path)
        slash[1] = '\0';
    else
        *slash = '\0';
    if (load_directory(path, parent) < 0) {
        free(path);
        return -1;
    }
    for (int index = 0; index < parent->count; ++index)
        if (strcmp(parent->entries[index]->d_name, name) == 0) {
            parent->selected = index;
            break;
        }
    return 0;
}

static bool extend_to_root(Directory **directories, size_t *depth)
{
    while (strcmp((*directories)[*depth - 1].path, "/") != 0) {
        Directory parent;
        if (load_parent_directory(&(*directories)[*depth - 1], &parent) < 0)
            return false;
        Directory *grown = realloc(*directories, (*depth + 1) * sizeof(**directories));
        if (!grown) {
            free_directory(&parent);
            return false;
        }
        *directories = grown;
        (*directories)[(*depth)++] = parent;
    }
    for (size_t index = 0; index < *depth / 2; ++index) {
        Directory swap = (*directories)[index];
        (*directories)[index] = (*directories)[*depth - 1 - index];
        (*directories)[*depth - 1 - index] = swap;
    }
    return true;
}

static int open_editor(const char *path)
{
    const char *editor = getenv("EDITOR");
    if (!editor || !*editor)
        return -1;
    struct termios raw;
    if (tcgetattr(STDIN_FILENO, &raw) < 0)
        return -1;
    wordexp_t words;
    if (wordexp(editor, &words, WRDE_NOCMD | WRDE_UNDEF) != 0)
        return -1;
    if (!words.we_wordc) {
        wordfree(&words);
        return -1;
    }
    char **arguments = calloc(words.we_wordc + 2, sizeof(*arguments));
    if (!arguments) {
        wordfree(&words);
        return -1;
    }
    for (size_t index = 0; index < words.we_wordc; ++index)
        arguments[index] = words.we_wordv[index];
    arguments[words.we_wordc] = (char *)path;

    term_pop_keyboard_mode();
    term_reset_style();
    term_show_cursor();
    fflush(stdout);
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_terminal) < 0) {
        term_hide_cursor();
        term_push_keyboard_mode();
        fflush(stdout);
        free(arguments);
        wordfree(&words);
        return -1;
    }
    terminal_active = false;
    struct sigaction ignore = {.sa_handler = SIG_IGN};
    struct sigaction previous_int;
    sigemptyset(&ignore.sa_mask);
    sigaction(SIGINT, &ignore, &previous_int);
    pid_t child = fork();
    if (child == 0) {
        sigaction(SIGINT, &previous_int, NULL);
        execvp(arguments[0], arguments);
        perror(arguments[0]);
        _exit(127);
    }
    int result = -1;
    if (child > 0) {
        int status = 0;
        pid_t waited;
        do {
            waited = waitpid(child, &status, 0);
        } while (waited < 0 && errno == EINTR);
        if (waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0)
            result = 0;
    }
    sigaction(SIGINT, &previous_int, NULL);
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0) {
        terminal_active = true;
        term_enter_alternate_buffer();
        fflush(stdout);
    }
    free(arguments);
    wordfree(&words);
    return result;
}

typedef enum { ENTRY_FILE, ENTRY_DIRECTORY } EntryType;

static void create_entry(Directory *current, EntryType type, char *message, size_t capacity)
{
    char name[256];
    if (!prompt_name(type == ENTRY_DIRECTORY ? "New directory: " : "New file: ", "", name, sizeof(name)))
        return;
    char *path = child_path(current->path, name);
    if (!path) {
        snprintf(message, capacity, "Out of memory");
        return;
    }
    int result;
    if (type == ENTRY_DIRECTORY) {
        result = mkdir(path, NEW_DIRECTORY_MODE);
    } else {
        int file = open(path, O_WRONLY | O_CREAT | O_EXCL, NEW_FILE_MODE);
        result = file < 0 ? -1 : close(file);
    }
    if (result < 0)
        snprintf(message, capacity, "Cannot create: %s", strerror(errno));
    else if (refresh_directory(current, name) < 0)
        snprintf(message, capacity, "Cannot refresh: %s", strerror(errno));
    free(path);
}

static void rename_entry(Directory *current, char *message, size_t capacity)
{
    const char *old_name = current->entries[current->selected]->d_name;
    char name[256];
    if (!prompt_name("Rename: ", old_name, name, sizeof(name)) || strcmp(name, old_name) == 0)
        return;
    char *old_path = child_path(current->path, old_name);
    char *new_path = child_path(current->path, name);
    if (!old_path || !new_path)
        snprintf(message, capacity, "Out of memory");
    else if (renameat2(AT_FDCWD, old_path, AT_FDCWD, new_path, RENAME_NOREPLACE) < 0)
        snprintf(message, capacity, "Cannot rename: %s", strerror(errno));
    else {
        rename_opened_paths(old_path, new_path);
        if (refresh_directory(current, name) < 0)
            snprintf(message, capacity, "Cannot refresh: %s", strerror(errno));
    }
    free(old_path);
    free(new_path);
}

static void remove_entry(Directory *current, char *message, size_t capacity)
{
    if (!confirm_remove(current->entries[current->selected]->d_name))
        return;
    char *path = child_path(current->path, current->entries[current->selected]->d_name);
    if (!path) {
        snprintf(message, capacity, "Out of memory");
        return;
    }
    struct stat info;
    if (lstat(path, &info) < 0 ||
        (S_ISDIR(info.st_mode) ? rmdir(path) : unlink(path)) < 0)
        snprintf(message, capacity, "Cannot remove: %s", strerror(errno));
    else if (refresh_directory(current, NULL) < 0)
        snprintf(message, capacity, "Cannot refresh: %s", strerror(errno));
    free(path);
}

static void edit_entry(Directory *current, char *message, size_t capacity)
{
    const char *name = current->entries[current->selected]->d_name;
    char *path = child_path(current->path, name);
    if (!path) {
        snprintf(message, capacity, "Out of memory");
        return;
    }
    if (open_editor(path) < 0)
        snprintf(message, capacity, "Editor unavailable or exited with an error");
    if (!terminal_active)
        stopped = 1;
    if (refresh_directory(current, name) < 0)
        snprintf(message, capacity, "Cannot refresh: %s", strerror(errno));
    free(path);
}

static char *choose_directory(const Directory *current)
{
    char *candidate = child_path(current->path, current->entries[current->selected]->d_name);
    struct stat info;
    char *chosen = NULL;
    if (candidate && stat(candidate, &info) == 0 && S_ISDIR(info.st_mode))
        chosen = realpath(candidate, NULL);
    free(candidate);
    return chosen;
}

static void leave_directory(Directory *directories, size_t *depth, size_t *base)
{
    if (*depth == 1)
        return;
    Directory *current = &directories[*depth - 1];
    if (*depth - 1 <= *base) {
        keep_open(current->path);
        --*base;
    }
    remember_selection(current);
    free_directory(current);
    --*depth;
}

static bool enter_directory(Directory **directories, size_t *depth, size_t *capacity,
                            char *message, size_t message_capacity)
{
    Directory *current = &(*directories)[*depth - 1];
    char *child = child_path(current->path, current->entries[current->selected]->d_name);
    if (!child) {
        snprintf(message, message_capacity, "Out of memory");
        return true;
    }
    struct stat info;
    if (stat(child, &info) < 0 || !S_ISDIR(info.st_mode)) {
        snprintf(message, message_capacity, "Not a readable directory");
        free(child);
        return true;
    }
    Directory next;
    if (load_directory(child, &next) < 0) {
        snprintf(message, message_capacity, "Cannot open: %s", strerror(errno));
        free(child);
        return true;
    }
    if (*depth == *capacity) {
        size_t new_capacity = *capacity * 2;
        Directory *grown = realloc(*directories, new_capacity * sizeof(**directories));
        if (!grown) {
            snprintf(message, message_capacity, "Out of memory");
            free_directory(&next);
            return false;
        }
        *directories = grown;
        *capacity = new_capacity;
    }
    (*directories)[(*depth)++] = next;
    keep_open(next.path);
    return true;
}

static void focus_view_root(size_t depth, size_t *base, bool *parent_visible)
{
    *base = depth - 1;
    *parent_visible = false;
}

int main(int argc, char **argv)
{
    bool cd_mode = argc > 1 && strcmp(argv[1], "--cd") == 0;
    if (argc > (cd_mode ? 3 : 2)) {
        fprintf(stderr, "Usage: %s [--cd] [directory]\n", argv[0]);
        return 1;
    }
    setlocale(LC_ALL, "");
    char *path = realpath(argc > (cd_mode ? 2 : 1) ? argv[argc - 1] : ".", NULL);
    if (!path) {
        perror("directory");
        return 1;
    }
    Directory *directories = malloc(sizeof(*directories));
    if (!directories || load_directory(path, directories) < 0) {
        perror(path);
        free(directories);
        free(path);
        return 1;
    }
    size_t depth = 1;
    if (!extend_to_root(&directories, &depth)) {
        perror("ancestors");
        for (size_t index = 0; index < depth; ++index)
            free_directory(&directories[index]);
        free(directories);
        return 1;
    }
    size_t capacity = depth;
    size_t base = depth - 1;
    int output_fd = -1;
    if (cd_mode) {
        output_fd = dup(STDOUT_FILENO);
        int terminal_fd = open("/dev/tty", O_WRONLY);
        if (output_fd < 0 || terminal_fd < 0 || dup2(terminal_fd, STDOUT_FILENO) < 0) {
            perror("terminal");
            if (output_fd >= 0)
                close(output_fd);
            if (terminal_fd >= 0)
                close(terminal_fd);
            for (size_t index = 0; index < depth; ++index)
                free_directory(&directories[index]);
            free(directories);
            return 1;
        }
        close(terminal_fd);
    }
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO) || tcgetattr(STDIN_FILENO, &original_terminal) < 0) {
        fprintf(stderr, "An interactive terminal is required.\n");
        for (size_t index = 0; index < depth; ++index)
            free_directory(&directories[index]);
        free(directories);
        if (output_fd >= 0)
            close(output_fd);
        return 1;
    }
    struct termios raw = original_terminal;
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_iflag &= (tcflag_t)~(IXON | ICRNL);
    raw.c_oflag &= (tcflag_t)~OPOST;
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0) {
        perror("tcsetattr");
        for (size_t index = 0; index < depth; ++index)
            free_directory(&directories[index]);
        free(directories);
        if (output_fd >= 0)
            close(output_fd);
        return 1;
    }
    terminal_active = true;
    atexit(restore_terminal);
    struct sigaction action = {.sa_handler = on_signal};
    sigemptyset(&action.sa_mask);
    sigaction(SIGWINCH, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
    term_enter_alternate_buffer();
    load_colors();

    bool parent_visible = false;
    char *chosen_directory = NULL;
    char message[256] = "";
    bool dirty = true;
    bool animate_depth = false;
    while (!stopped) {
        if (dirty || resized) {
            if (resized) {
                free(painted_cells);
                painted_cells = NULL;
            }
            size_t first = parent_visible ? 0 : base;
            const Directory *visible = &directories[first];
            size_t visible_depth = depth - first;
            bool animate = animate_depth && !resized;
            long target = draw(visible, visible_depth, message, animate, true);
            dirty = false;
            resized = 0;
            if (animate)
                scroll_depth(visible, visible_depth, message, target);
            animate_depth = false;
        }
        int key = read_key();
        if (key == '?') {
            show_keys(cd_mode);
            free(painted_cells);
            painted_cells = NULL;
            term_clear_screen();
            dirty = true;
            continue;
        }
        if (key == -1 || key == 'q' || key == 27 || key == 3)
            break;
        if (!key)
            continue;
        char previous_message[sizeof(message)];
        memcpy(previous_message, message, sizeof(message));
        bool had_message = message[0] != '\0';
        message[0] = '\0';
        size_t previous_depth = depth;
        Directory *current = &directories[depth - 1];
        int previous = current->selected;
        if (key == KEY_UP && current->selected > 0)
            --current->selected;
        else if (key == KEY_DOWN && current->selected + 1 < current->count)
            ++current->selected;
        else if (key == ' ' && current->count) {
            char *child = child_path(current->path, current->entries[current->selected]->d_name);
            if (child) {
                struct stat info;
                if (stat(child, &info) == 0 && S_ISDIR(info.st_mode))
                    toggle_open(child);
                else
                    free(child);
            }
        }
        else if (key == KEY_SHIFT_SPACE || key == 'H')
            focus_view_root(depth, &base, &parent_visible);
        else if (key == 'A')
            parent_visible = !parent_visible;
        else if (key == 'n' || key == 'N')
            create_entry(current, key == 'N' ? ENTRY_DIRECTORY : ENTRY_FILE,
                         message, sizeof(message));
        else if (key == 'r' && current->count)
            rename_entry(current, message, sizeof(message));
        else if (key == 'd' && current->count)
            remove_entry(current, message, sizeof(message));
        else if ((key == 'e' || key == '\r' || key == '\n') && current->count)
            edit_entry(current, message, sizeof(message));
        else if (key == 'c' && cd_mode && current->count) {
            chosen_directory = choose_directory(current);
            if (!chosen_directory)
                snprintf(message, sizeof(message), "Select a directory to change into");
            if (chosen_directory)
                break;
        }
        else if (key == KEY_LEFT)
            leave_directory(directories, &depth, &base);
        else if (key == KEY_RIGHT && current->count &&
                 !enter_directory(&directories, &depth, &capacity, message, sizeof(message))) {
            dirty = true;
            continue;
        }
        bool selection_changed = (key == KEY_UP || key == KEY_DOWN) &&
                                 current->selected != previous;
        bool directory_changed = depth != previous_depth;
        if (directory_changed)
            animate_depth = true;
        bool contents_may_change = key == 'n' || key == 'N' || key == 'r' || key == 'd' ||
                                   key == 'e' || key == '\r' || key == '\n';
        if (directory_changed || contents_may_change || key == ' ')
            clear_directory_cache();
        if (selection_changed || directory_changed || key == ' ' || contents_may_change) {
            dirty = true;
        }
        if (key == KEY_SHIFT_SPACE || key == 'H' || key == 'A')
            dirty = true;
        if (key == KEY_UP || key == KEY_DOWN) {
            if (current->selected == previous && !had_message && !resized && !dirty)
                continue;
        }
        if ((key == KEY_RIGHT || key == KEY_LEFT) &&
            depth == previous_depth && strcmp(message, previous_message) == 0 && !resized && !dirty)
            continue;
        dirty = true;
    }
    free(painted_cells);
    for (size_t index = 0; index < focused_count; ++index) {
        free(focused_entries[index].path);
        free(focused_entries[index].entry);
    }
    free(focused_entries);
    clear_directory_cache();
    for (size_t index = 0; index < opened_count; ++index)
        free(opened_paths[index]);
    free(opened_paths);
    for (size_t index = 0; index < depth; ++index)
        free_directory(&directories[index]);
    free(directories);
    free(color_rules);
    free(color_storage);
    restore_terminal();
    int result = chosen_directory && dprintf(output_fd, "%s\n", chosen_directory) < 0 ? 1 : 0;
    free(chosen_directory);
    if (output_fd >= 0)
        close(output_fd);
    return result;
}