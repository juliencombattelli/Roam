#define _GNU_SOURCE
#define _XOPEN_SOURCE 700

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
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
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <wchar.h>
#include <wordexp.h>

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

static void term_exit_alternate_buffer(void)
{
    fputs("\033[0m\033[?25h\033[?1049l", stdout);
}

static void term_enter_alternate_buffer(void)
{
    fputs("\033[?1049h\033[?25l", stdout);
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

static void term_set_color(const char *color)
{
    printf("\033[%sm", color);
}

static void term_reset_style(void)
{
    fputs("\033[0m", stdout);
}

static void term_bold(void)
{
    fputs("\033[1m", stdout);
}

static void term_dim(void)
{
    fputs("\033[2m", stdout);
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
    term_move_cursor(row, column);
    fputs(index == directory->selected ? "►" : " ", stdout);
    const char *color = color_rule_count
        ? entry_color(directory, directory->entries[index]->d_name) : NULL;
    if (color)
        term_set_color(color);
    if (active && index == directory->selected) {
        term_reverse_video();
    } else if (index == directory->selected) {
        term_bold();
    }
    clipped_text(directory->entries[index]->d_name, 0, width - 1);
    term_reset_style();
}

static int link_start(const Directory *directory, int column, int width)
{
    const char *name = directory->entries[directory->selected]->d_name;
    int start = column + 1 + text_width(name, width - 1);
    return start < 0 ? 0 : start;
}

static void draw_link(int start, int end, int parent_row, int child_row, bool erase)
{
    if (start > end - 2)
        start = end - 2;
    term_move_cursor(parent_row, start);
    int bend = end - 2;
    for (int column = start; column < (parent_row == child_row ? end : bend); ++column)
        fputs(erase ? " " : "─", stdout);
    if (parent_row == child_row)
        return;
    fputs(erase ? " " : parent_row < child_row ? "┐" : "┘", stdout);
    for (int row = parent_row - 1; row > child_row; --row) {
        term_move_cursor(row, bend);
        fputs(erase ? " " : "│", stdout);
    }
    for (int row = parent_row + 1; row < child_row; ++row) {
        term_move_cursor(row, bend);
        fputs(erase ? " " : "│", stdout);
    }
    term_move_cursor(child_row, bend);
    fputs(erase ? "  " : parent_row < child_row ? "└─" : "┌─", stdout);
}

static void draw(const Directory *directories, size_t depth, const Directory *preview,
                 const char *message, bool cd_mode)
{
    struct winsize size;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) < 0 || !size.ws_col || !size.ws_row)
        return;
    int columns = size.ws_col;
    int rows = size.ws_row;
    term_clear_screen();
    if (columns < 12 || rows < 6) {
        term_move_cursor(0, 0);
        text("Terminal too small", columns);
        fflush(stdout);
        return;
    }

    const Directory *active = &directories[depth - 1];
    term_move_cursor(0, 0);
    term_bold();
    text(active->path, columns);
    term_reset_style();

    int available = rows - 5;
    long position = 0;
    int child_row = -1;
    for (size_t step = depth + (preview != NULL); step > 0; --step) {
        size_t index = step - 1;
        const Directory *directory = step > depth ? preview : &directories[index];
        int width = directory_width(directory, columns - 2);
        if (step > depth)
            position = (columns - directory_width(active, columns - 2)) / 2 +
                       directory_width(active, columns - 2) + 4;
        else if (step == depth)
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
        term_move_cursor(2, left);
        term_bold();
        clipped_text(title, skip, visible_width);
        term_reset_style();

        int first = first_visible(directory, available, directory->selected);
        int visible = directory->count < available ? directory->count : available;
        int top = directory->count < available ? 3 + (available - visible) / 2 : 3;
        int selected_row = focus_row(directory, available, directory->selected);
        if (!directory->count) {
            term_move_cursor(3 + available / 2, left);
            clipped_text("(empty)", skip, visible_width);
        }
        for (int offset = 0; offset < visible; ++offset) {
            int entry_index = first + offset;
            int row = top + offset;
            draw_entry(directory, entry_index, row, left, visible_width, index == depth - 1);
        }
        if (child_row >= 0)
            draw_link(link_start(directory, column, width), column + width + 4,
                      selected_row, child_row, false);
        child_row = selected_row;
    }
    term_move_cursor(rows - 2, 0);
    if (*message)
        text(message, columns);
    term_move_cursor(rows - 1, 0);
    term_dim();
    text(cd_mode ? "Arrows browse  Space preview  e edit  n/N new  r rename  d delete  c cd  q quit"
                 : "Arrows browse  Space preview  e edit  n/N new  r rename  d delete  q quit", columns);
    term_reset_style();
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
            draw_link(link_start(parent, parent_column, parent_width), column,
                      parent_row, focus_row(active, available, previous), true);
        }
    }
    if (old_first == first) {
        int old_row = top + previous - first;
        int new_row = top + active->selected - first;
        term_move_cursor(old_row, column);
        printf("%-*s", width, "");
        draw_entry(active, previous, old_row, column, width, true);
        term_move_cursor(new_row, column);
        printf("%-*s", width, "");
        draw_entry(active, active->selected, new_row, column, width, true);
    } else {
        for (int offset = 0; offset < visible; ++offset) {
            term_move_cursor(top + offset, column);
            printf("%-*s", width, "");
            draw_entry(active, first + offset, top + offset, column, width, true);
        }
    }
    if (depth > 1) {
        const Directory *parent = &directories[depth - 2];
        if (column - 4 > 0 && column - 4 - directory_width(parent, columns - 2) < columns)
            draw_link(link_start(parent, column - directory_width(parent, columns - 2) - 4,
                                 directory_width(parent, columns - 2)), column,
                      focus_row(parent, available, parent->selected),
                      focus_row(active, available, active->selected), false);
    }
    term_move_cursor(size.ws_row - 2, 0);
    term_clear_line();
    fflush(stdout);
    return true;
}

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
    if (read(STDIN_FILENO, &prefix, 1) != 1)
        return 0;
    if (prefix != '[' && prefix != 'O') {
        pending_key = prefix;
        return 27;
    }
    timeout.tv_usec = 50000;
    FD_ZERO(&input);
    FD_SET(STDIN_FILENO, &input);
    if (select(STDIN_FILENO + 1, &input, NULL, NULL, &timeout) <= 0)
        return 0;
    if (read(STDIN_FILENO, &key, 1) != 1)
        return 0;
    return key >= 'A' && key <= 'D' ? key : 0;
}

static bool prompt_name(const char *label, const char *initial, char *name, size_t capacity)
{
    struct winsize size;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) < 0 || size.ws_row < 6)
        return false;
    size_t length = strlen(initial);
    if (length >= capacity)
        return false;
    memcpy(name, initial, length + 1);
    bool dirty = true;
    while (!stopped) {
        if (dirty) {
            term_move_cursor(size.ws_row - 2, 0);
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
        } else if (key >= 32 && key != 127 && key != '/' && length + 1 < capacity) {
            name[length++] = (char)key;
            name[length] = '\0';
            dirty = true;
        }
    }
    return false;
}

static bool confirm_remove(const char *name)
{
    struct winsize size;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) < 0 || size.ws_row < 6)
        return false;
    term_move_cursor(size.ws_row - 2, 0);
    term_clear_line();
    if (size.ws_col < 16) {
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

static void refresh_preview(Directory *preview, const Directory *current)
{
    if (preview->path)
        free_directory(preview);
    *preview = (Directory){0};
    if (!current->count)
        return;
    char *child = child_path(current->path, current->entries[current->selected]->d_name);
    if (!child)
        return;
    struct stat info;
    if (stat(child, &info) == 0 && S_ISDIR(info.st_mode) &&
        load_directory(child, preview) == 0)
        return;
    free(child);
}

static int open_editor(const char *path, const struct termios *raw)
{
    const char *editor = getenv("EDITOR");
    if (!editor || !*editor)
        return -1;
    wordexp_t words;
    if (wordexp(editor, &words, WRDE_NOCMD | WRDE_UNDEF) != 0)
        return -1;
    char **arguments = calloc(words.we_wordc + 2, sizeof(*arguments));
    if (!arguments || !words.we_wordc) {
        free(arguments);
        wordfree(&words);
        return -1;
    }
    for (size_t index = 0; index < words.we_wordc; ++index)
        arguments[index] = words.we_wordv[index];
    arguments[words.we_wordc] = (char *)path;

    term_exit_alternate_buffer();
    fflush(stdout);
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_terminal) < 0) {
        term_enter_alternate_buffer();
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
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, raw) == 0) {
        terminal_active = true;
        term_enter_alternate_buffer();
        fflush(stdout);
    }
    free(arguments);
    wordfree(&words);
    return result;
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
            free_directory(directories);
            free(directories);
            return 1;
        }
        close(terminal_fd);
    }
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO) || tcgetattr(STDIN_FILENO, &original_terminal) < 0) {
        fprintf(stderr, "An interactive terminal is required.\n");
        free_directory(directories);
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
        free_directory(directories);
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

    size_t depth = 1;
    size_t capacity = 1;
    Directory preview = {0};
    bool preview_enabled = false;
    char *chosen_directory = NULL;
    char message[256] = "";
    bool dirty = true;
    while (!stopped) {
        if (dirty || resized) {
            draw(directories, depth, preview.path ? &preview : NULL, message, cd_mode);
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
        else if (key == ' ') {
            preview_enabled = !preview_enabled;
            if (!preview_enabled && preview.path) {
                free_directory(&preview);
                preview = (Directory){0};
            }
        }
        else if (key == 'n' || key == 'N') {
            char name[256];
            if (prompt_name(key == 'n' ? "New file: " : "New directory: ", "", name, sizeof(name))) {
                char *path = child_path(current->path, name);
                if (!path) {
                    snprintf(message, sizeof(message), "Out of memory");
                } else {
                    int result;
                    if (key == 'N') {
                        result = mkdir(path, 0777);
                    } else {
                        int file = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
                        result = file < 0 ? -1 : close(file);
                    }
                    if (result < 0)
                        snprintf(message, sizeof(message), "Cannot create: %s", strerror(errno));
                    else if (refresh_directory(current, name) < 0)
                        snprintf(message, sizeof(message), "Cannot refresh: %s", strerror(errno));
                    free(path);
                }
            }
        }
        else if (key == 'r' && current->count) {
            const char *old_name = current->entries[current->selected]->d_name;
            char name[256];
            if (prompt_name("Rename: ", old_name, name, sizeof(name)) && strcmp(name, old_name) != 0) {
                char *old_path = child_path(current->path, old_name);
                char *new_path = child_path(current->path, name);
                if (!old_path || !new_path) {
                    snprintf(message, sizeof(message), "Out of memory");
                } else if (renameat2(AT_FDCWD, old_path, AT_FDCWD, new_path, RENAME_NOREPLACE) < 0) {
                    snprintf(message, sizeof(message), "Cannot rename: %s", strerror(errno));
                } else if (refresh_directory(current, name) < 0) {
                    snprintf(message, sizeof(message), "Cannot refresh: %s", strerror(errno));
                }
                free(old_path);
                free(new_path);
            }
        }
        else if (key == 'd' && current->count &&
                 confirm_remove(current->entries[current->selected]->d_name)) {
            char *path = child_path(current->path, current->entries[current->selected]->d_name);
            if (!path) {
                snprintf(message, sizeof(message), "Out of memory");
            } else {
                struct stat info;
                if (lstat(path, &info) < 0 ||
                    (S_ISDIR(info.st_mode) ? rmdir(path) : unlink(path)) < 0)
                    snprintf(message, sizeof(message), "Cannot remove: %s", strerror(errno));
                else if (refresh_directory(current, NULL) < 0)
                    snprintf(message, sizeof(message), "Cannot refresh: %s", strerror(errno));
                free(path);
            }
        }
        else if ((key == 'e' || key == '\r' || key == '\n') && current->count) {
            const char *name = current->entries[current->selected]->d_name;
            char *path = child_path(current->path, name);
            if (!path) {
                snprintf(message, sizeof(message), "Out of memory");
            } else {
                if (open_editor(path, &raw) < 0)
                    snprintf(message, sizeof(message), "Editor unavailable or exited with an error");
                if (!terminal_active)
                    stopped = 1;
                if (refresh_directory(current, name) < 0)
                    snprintf(message, sizeof(message), "Cannot refresh: %s", strerror(errno));
                free(path);
            }
        }
        else if (key == 'c' && cd_mode && current->count) {
            char *candidate = child_path(current->path, current->entries[current->selected]->d_name);
            struct stat info;
            if (candidate && stat(candidate, &info) == 0 && S_ISDIR(info.st_mode))
                chosen_directory = realpath(candidate, NULL);
            if (!chosen_directory)
                snprintf(message, sizeof(message), "Select a directory to change into");
            free(candidate);
            if (chosen_directory)
                break;
        }
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
        if (preview_enabled) {
            refresh_preview(&preview, &directories[depth - 1]);
            dirty = true;
        }
        if (key == 'A' || key == 'B' || key == 'j' || key == 'k') {
            if (current->selected == previous && !had_message && !resized && !dirty)
                continue;
            if (current->selected != previous && !resized && !dirty &&
                redraw_focus(directories, depth, previous))
                continue;
        }
        if ((key == 'C' || key == 'D' || key == 'h' || key == 'l') &&
            depth == previous_depth && strcmp(message, previous_message) == 0 && !resized && !dirty)
            continue;
        dirty = true;
    }
    if (preview.path)
        free_directory(&preview);
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