#define _XOPEN_SOURCE 700

#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <limits.h>
#include <locale.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#include <wchar.h>

typedef struct {
    char *path;
    struct dirent **entries;
    int count;
    int selected;
    int width;
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

static void restore_terminal(void)
{
    if (terminal_active) {
        printf("\033[0m\033[?25h\033[?1049l");
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
    const char *title = strrchr(path, '/');
    title = title && title[1] ? title + 1 : path;
    int width = text_width(title, INT_MAX - 4);
    if (!count && width < 7)
        width = 7;
    for (int index = 0; index < count; ++index) {
        int entry_width = 1 + text_width(entries[index]->d_name, INT_MAX - 4);
        if (entry_width > width)
            width = entry_width;
    }
    *directory = (Directory){.path = path, .entries = entries, .count = count,
                             .width = width < 2 ? 2 : width};
    return 0;
}

static void free_directory(Directory *directory)
{
    for (int index = 0; index < directory->count; ++index)
        free(directory->entries[index]);
    free(directory->entries);
    free(directory->path);
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
    char *save = NULL;
    for (char *part = strtok_r(color_storage, ":", &save); part;
         part = strtok_r(NULL, ":", &save)) {
        char *separator = strchr(part, '=');
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

static const char *entry_color(const Directory *directory, const char *name)
{
    char *path = child_path(directory->path, name);
    if (!path)
        return NULL;
    struct stat info;
    const char *color = NULL;
    if (lstat(path, &info) < 0) {
        color = color_rule("mi");
        free(path);
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
            free(path);
            return color;
        }
        if (color || !link_color || strcmp(link_color, "target") != 0) {
            free(path);
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
    free(path);
    return color;
}

static void at(int row, int column)
{
    printf("\033[%d;%dH", row + 1, column + 1);
}

static void clipped_text(const char *source, int skip, int width)
{
    mbstate_t state = {0};
    while (*source && width > 0) {
        wchar_t character;
        size_t bytes = mbrtowc(&character, source, MB_CUR_MAX, &state);
        if (bytes == (size_t)-1 || bytes == (size_t)-2 || bytes == 0) {
            if (skip <= 0)
                putchar('?');
            else
                --skip;
            ++source;
            if (skip <= 0)
                --width;
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

static int directory_width(const Directory *directory, int limit)
{
    return directory->width < limit ? directory->width : limit;
}

static void entry_text(const char *name, bool selected, int skip, int width)
{
    if (skip == 0) {
        putchar(selected ? '>' : ' ');
        --width;
    } else {
        --skip;
    }
    clipped_text(name, skip, width);
}

static int first_visible(const Directory *directory, int available, int selected)
{
    int first = directory->count > available ? selected - available / 2 : 0;
    if (first < 0)
        first = 0;
    if (first > directory->count - available)
        first = directory->count - available;
    return first < 0 ? 0 : first;
}

static int focus_row(const Directory *directory, int available, int selected)
{
    int visible = directory->count < available ? directory->count : available;
    int top = directory->count < available ? 3 + (available - visible) / 2 : 3;
    return directory->count ? top + selected - first_visible(directory, available, selected)
                            : 3 + available / 2;
}

static void draw_entry(const Directory *directory, int index, int row, int column,
                       int width, bool active)
{
    at(row, column);
    const char *color = color_rule_count
        ? entry_color(directory, directory->entries[index]->d_name) : NULL;
    if (color)
        printf("\033[%sm", color);
    if (active && index == directory->selected) {
        printf("\033[7m");
        printf("%-*s", width, "");
        at(row, column);
    } else if (index == directory->selected) {
        printf("\033[1m");
    }
    entry_text(directory->entries[index]->d_name, index == directory->selected, 0, width);
    printf("\033[0m");
}

static void draw_link(int column, int parent_row, int child_row, bool erase)
{
    at(parent_row, column);
    fputs(erase ? "    " : parent_row == child_row ? "────" :
          parent_row < child_row ? "─┐" : "─┘", stdout);
    if (parent_row == child_row)
        return;
    for (int row = parent_row - 1; row > child_row; --row) {
        at(row, column + 1);
        fputs(erase ? " " : "│", stdout);
    }
    for (int row = parent_row + 1; row < child_row; ++row) {
        at(row, column + 1);
        fputs(erase ? " " : "│", stdout);
    }
    at(child_row, column + 1);
    fputs(erase ? "   " : parent_row < child_row ? "└──" : "┌──", stdout);
}

static void draw(const Directory *directories, size_t depth, const char *message)
{
    struct winsize size;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) < 0 || !size.ws_col || !size.ws_row)
        return;
    int columns = size.ws_col;
    int rows = size.ws_row;
    printf("\033[H\033[2J");
    if (columns < 12 || rows < 6) {
        at(0, 0);
        text("Terminal too small", columns);
        fflush(stdout);
        return;
    }

    const Directory *active = &directories[depth - 1];
    at(0, 0);
    printf("\033[1m");
    text(active->path, columns);
    printf("\033[0m");

    int available = rows - 5;
    long position = 0;
    int child_row = -1;
    for (size_t step = depth; step > 0; --step) {
        size_t index = step - 1;
        const Directory *directory = &directories[index];
        int width = directory_width(directory, columns - 2);
        if (step == depth)
            position = (columns - width) / 2;
        else
            position -= width + 4;
        if (position + width <= 0 || position >= columns) {
            child_row = -1;
            continue;
        }
        int column = (int)position;
        int left = column < 0 ? 0 : column;
        int skip = 0;
        int visible_width = column < 0 ? column + width : width;
        if (visible_width > columns - left)
            visible_width = columns - left;
        const char *title = strrchr(directory->path, '/');
        title = title && title[1] ? title + 1 : directory->path;
        at(2, left);
        printf("\033[1m");
        clipped_text(title, skip, visible_width);
        printf("\033[0m");

        int first = first_visible(directory, available, directory->selected);
        int visible = directory->count < available ? directory->count : available;
        int top = directory->count < available ? 3 + (available - visible) / 2 : 3;
        int selected_row = focus_row(directory, available, directory->selected);
        if (!directory->count) {
            at(3 + available / 2, left);
            clipped_text("(empty)", skip, visible_width);
        }
        for (int offset = 0; offset < visible; ++offset) {
            int entry_index = first + offset;
            int row = top + offset;
            draw_entry(directory, entry_index, row, left, visible_width, index == depth - 1);
        }
        if (child_row >= 0)
            draw_link(column + width, selected_row, child_row, false);
        child_row = selected_row;
    }
    at(rows - 2, 0);
    if (*message)
        text(message, columns);
    at(rows - 1, 0);
    printf("\033[2m");
    text("Up/Down: select  Right: open  Left: back  q: quit", columns);
    printf("\033[0m");
    fflush(stdout);
}

static bool redraw_focus(const Directory *directories, size_t depth, int previous)
{
    struct winsize size;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) < 0 || size.ws_col < 12 || size.ws_row < 6)
        return false;
    const Directory *active = &directories[depth - 1];
    int columns = size.ws_col;
    int available = size.ws_row - 5;
    int width = directory_width(active, columns - 2);
    int column = (columns - width) / 2;
    int old_first = first_visible(active, available, previous);
    int first = first_visible(active, available, active->selected);
    int visible = active->count < available ? active->count : available;
    int top = active->count < available ? 3 + (available - visible) / 2 : 3;

    if (depth > 1) {
        const Directory *parent = &directories[depth - 2];
        int parent_width = directory_width(parent, columns - 2);
        int parent_column = column - parent_width - 4;
        if (parent_column + parent_width > 0) {
            int parent_row = focus_row(parent, available, parent->selected);
            draw_link(column - 4, parent_row, focus_row(active, available, previous), true);
        }
    }
    if (old_first == first) {
        int old_row = top + previous - first;
        int new_row = top + active->selected - first;
        at(old_row, column);
        printf("%-*s", width, "");
        draw_entry(active, previous, old_row, column, width, true);
        at(new_row, column);
        printf("%-*s", width, "");
        draw_entry(active, active->selected, new_row, column, width, true);
    } else {
        for (int offset = 0; offset < visible; ++offset) {
            at(top + offset, column);
            printf("%-*s", width, "");
            draw_entry(active, first + offset, top + offset, column, width, true);
        }
    }
    if (depth > 1) {
        const Directory *parent = &directories[depth - 2];
        if (column - 4 > 0 && column - 4 - directory_width(parent, columns - 2) < columns)
            draw_link(column - 4, focus_row(parent, available, parent->selected),
                      focus_row(active, available, active->selected), false);
    }
    at(size.ws_row - 2, 0);
    fputs("\033[2K", stdout);
    fflush(stdout);
    return true;
}

static int read_key(void)
{
    fd_set input;
    FD_ZERO(&input);
    FD_SET(STDIN_FILENO, &input);
    struct timeval timeout = {.tv_usec = 200000};
    int ready = select(STDIN_FILENO + 1, &input, NULL, NULL, &timeout);
    if (ready <= 0)
        return 0;
    unsigned char key;
    if (read(STDIN_FILENO, &key, 1) != 1)
        return -1;
    if (key != 27)
        return key;

    timeout.tv_usec = 50000;
    FD_ZERO(&input);
    FD_SET(STDIN_FILENO, &input);
    if (select(STDIN_FILENO + 1, &input, NULL, NULL, &timeout) <= 0)
        return 27;
    unsigned char prefix;
    if (read(STDIN_FILENO, &prefix, 1) != 1 || (prefix != '[' && prefix != 'O'))
        return 0;
    timeout.tv_usec = 50000;
    FD_ZERO(&input);
    FD_SET(STDIN_FILENO, &input);
    if (select(STDIN_FILENO + 1, &input, NULL, NULL, &timeout) <= 0)
        return 0;
    if (read(STDIN_FILENO, &key, 1) != 1)
        return 0;
    return key >= 'A' && key <= 'D' ? key : 0;
}

int main(int argc, char **argv)
{
    if (argc > 2) {
        fprintf(stderr, "Usage: %s [directory]\n", argv[0]);
        return 1;
    }
    setlocale(LC_ALL, "");
    char *path = realpath(argc == 2 ? argv[1] : ".", NULL);
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
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO) || tcgetattr(STDIN_FILENO, &original_terminal) < 0) {
        fprintf(stderr, "An interactive terminal is required.\n");
        free_directory(directories);
        free(directories);
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
        free_directory(directories);
        free(directories);
        return 1;
    }
    terminal_active = true;
    atexit(restore_terminal);
    struct sigaction action = {.sa_handler = on_signal};
    sigemptyset(&action.sa_mask);
    sigaction(SIGWINCH, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
    printf("\033[?1049h\033[?25l");
    load_colors();

    size_t depth = 1;
    size_t capacity = 1;
    char message[256] = "";
    bool dirty = true;
    while (!stopped) {
        if (dirty || resized) {
            draw(directories, depth, message);
            dirty = false;
            resized = 0;
        }
        int key = read_key();
        if (key == -1 || key == 'q' || key == 3)
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
        if ((key == 'A' || key == 'k') && current->selected > 0)
            --current->selected;
        else if ((key == 'B' || key == 'j') && current->selected + 1 < current->count)
            ++current->selected;
        else if ((key == 'D' || key == 'h') && depth > 1) {
            free_directory(current);
            --depth;
        } else if ((key == 'C' || key == 'l') && current->count) {
            char *child = child_path(current->path, current->entries[current->selected]->d_name);
            if (!child) {
                snprintf(message, sizeof(message), "Out of memory");
            } else {
                struct stat info;
                if (stat(child, &info) < 0 || !S_ISDIR(info.st_mode)) {
                    snprintf(message, sizeof(message), "Not a readable directory");
                    free(child);
                } else {
                    Directory next;
                    if (load_directory(child, &next) < 0) {
                        snprintf(message, sizeof(message), "Cannot open: %s", strerror(errno));
                        free(child);
                    } else {
                        if (depth == capacity) {
                            size_t new_capacity = capacity * 2;
                            Directory *grown = realloc(directories, new_capacity * sizeof(*directories));
                            if (!grown) {
                                snprintf(message, sizeof(message), "Out of memory");
                                free_directory(&next);
                                dirty = true;
                                continue;
                            }
                            directories = grown;
                            capacity = new_capacity;
                        }
                        directories[depth++] = next;
                    }
                }
            }
        }
        if (key == 'A' || key == 'B' || key == 'j' || key == 'k') {
            if (current->selected == previous && !had_message && !resized)
                continue;
            if (current->selected != previous && !resized &&
                redraw_focus(directories, depth, previous))
                continue;
        }
        if ((key == 'C' || key == 'D' || key == 'h' || key == 'l') &&
            depth == previous_depth && strcmp(message, previous_message) == 0 && !resized)
            continue;
        dirty = true;
    }
    for (size_t index = 0; index < depth; ++index)
        free_directory(&directories[index]);
    free(directories);
    free(color_rules);
    free(color_storage);
    return 0;
}