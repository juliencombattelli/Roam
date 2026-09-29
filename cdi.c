#define _XOPEN_SOURCE 700

#include <dirent.h>
#include <errno.h>
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
} Directory;

static struct termios original_terminal;
static bool terminal_active;
static volatile sig_atomic_t stopped;
static volatile sig_atomic_t resized;

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

static int load_directory(char *path, Directory *directory)
{
    struct dirent **entries = NULL;
    int count = scandir(path, &entries, accept_entry, alphasort);
    if (count < 0)
        return -1;
    *directory = (Directory){.path = path, .entries = entries, .count = count};
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
    const char *title = strrchr(directory->path, '/');
    title = title && title[1] ? title + 1 : directory->path;
    int width = text_width(title, limit);
    if (!directory->count && width < 7)
        width = 7;
    for (int index = 0; index < directory->count && width < limit; ++index) {
        int entry_width = 1 + text_width(directory->entries[index]->d_name, limit - 1);
        if (entry_width > width)
            width = entry_width;
    }
    return width < 2 ? 2 : width;
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
    for (size_t step = depth; step > 0; --step) {
        size_t index = step - 1;
        const Directory *directory = &directories[index];
        int width = directory_width(directory, columns - 2);
        if (step == depth)
            position = (columns - width) / 2;
        else
            position -= width + 2;
        if (position + width <= 0 || position >= columns)
            continue;
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

        int first = directory->count > available ? directory->selected - available / 2 : 0;
        if (first < 0)
            first = 0;
        if (first > directory->count - available)
            first = directory->count - available;
        if (first < 0)
            first = 0;
        int visible = directory->count < available ? directory->count : available;
        int top = directory->count < available ? 3 + (available - visible) / 2 : 3;
        if (!directory->count) {
            at(3 + available / 2, left);
            clipped_text("(empty)", skip, visible_width);
        }
        for (int offset = 0; offset < visible; ++offset) {
            int entry_index = first + offset;
            int row = top + offset;
            at(row, left);
            if (index == depth - 1 && entry_index == directory->selected) {
                printf("\033[7m");
                printf("%-*s", visible_width, "");
                at(row, left);
            } else if (entry_index == directory->selected) {
                printf("\033[1m");
            }
            entry_text(directory->entries[entry_index]->d_name,
                       entry_index == directory->selected, skip, visible_width);
            printf("\033[0m");
        }
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
        message[0] = '\0';
        Directory *current = &directories[depth - 1];
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
        dirty = true;
    }
    for (size_t index = 0; index < depth; ++index)
        free_directory(&directories[index]);
    free(directories);
    return 0;
}