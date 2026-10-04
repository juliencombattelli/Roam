#+build linux
package main

import "core:fmt"
import "core:os"
import "core:sys/posix"

KEY_IDLE_TIMEOUT_USEC     :: 200_000
KEY_SEQUENCE_TIMEOUT_USEC :: 50_000

pending_key: int

read_one :: proc(timeout_usec: int) -> (value: byte, status: int) {
    readable: posix.fd_set
    posix.FD_ZERO(&readable)
    posix.FD_SET(posix.FD(posix.STDIN_FILENO), &readable)
    timeout := posix.timeval{
        tv_sec  = 0,
        tv_usec = posix.suseconds_t(timeout_usec),
    }
    ready := posix.select(posix.STDIN_FILENO+1, &readable, nil, nil, &timeout)
    if ready < 0 {
        return 0, -1
    }
    if ready == 0 {
        return 0, 0
    }
    input: [1]byte
    count := posix.read(posix.FD(posix.STDIN_FILENO), &input[0], 1)
    if count != 1 {
        return 0, -1
    }
    return input[0], 1
}

read_key :: proc() -> int {
    if pending_key != 0 {
        key := pending_key
        pending_key = 0
        return key
    }
    key, status := read_one(KEY_IDLE_TIMEOUT_USEC)
    if status < 0 {
        return -1
    }
    if status == 0 {
        return 0
    }
    if key != 27 {
        return int(key)
    }

    prefix: byte
    prefix, status = read_one(KEY_SEQUENCE_TIMEOUT_USEC)
    if status == 0 {
        return 27
    }
    if status < 0 {
        return -1
    }
    if prefix != '[' && prefix != 'O' {
        pending_key = int(prefix)
        return 27
    }

    first: byte
    first, status = read_one(KEY_SEQUENCE_TIMEOUT_USEC)
    if status <= 0 {
        return 0
    }
    if prefix != '[' || first < '0' || first > '9' {
        return arrow_key(first)
    }

    sequence: [64]byte
    length := 0
    sequence[length] = first
    length += 1
    for length < len(sequence)-1 {
        key, status = read_one(KEY_SEQUENCE_TIMEOUT_USEC)
        if status <= 0 {
            return 0
        }
        sequence[length] = key
        length += 1
        if key >= 0x40 && key <= 0x7e {
            break
        }
    }
    return decode_key_sequence(string(sequence[:length]))
}

action_for_key :: proc(key: int) -> string {
    switch key {
    case KEY_UP:
        return "Move selection up"
    case KEY_DOWN:
        return "Move selection down"
    case KEY_RIGHT:
        return "Open selected directory"
    case KEY_LEFT:
        return "Open parent directory"
    case ' ':
        return "Toggle directory folding"
    case KEY_SHIFT_SPACE, 'H':
        return "Hide ancestors"
    case 'A':
        return "Toggle ancestor visibility"
    case 'e', '\r', '\n':
        return "Edit selected entry (stub)"
    case 'n':
        return "Create file (stub)"
    case 'N':
        return "Create directory (stub)"
    case 'r':
        return "Rename selected entry (stub)"
    case 'd':
        return "Remove selected entry (stub)"
    case 'c':
        return "Choose directory (stub)"
    case '?':
        return "Show key help (stub)"
    }
    return ""
}

draw_screen :: proc(message: string) {
    fmt.println("\x1b[2J\x1b[H")
    fmt.println("roam Odin port")
    fmt.println("Filesystem and editor actions are disabled during this port.")
    fmt.println("")
    fmt.println("Arrow keys: navigate   Space: fold   H / Shift+Space: hide parents   A: show parents")
    fmt.println("e / Enter: edit   n: new file   N: new directory   r: rename   d: remove   c: choose")
    fmt.println("? : help   q / Escape / Ctrl-C: quit")
    fmt.println("")
    if message != "" {
        fmt.println("Action executed: ", message)
    } else {
        fmt.println("Ready")
    }
    fmt.println("")
    fmt.println("Press a key to continue.")
}

term_prepare :: proc() -> (original_termios: posix.termios, ok: bool = true) {
    if posix.tcgetattr(posix.FD(posix.STDIN_FILENO), &original_termios) != .OK {
        fmt.println("Cannot read terminal settings.")
        ok = false
        return
    }
    raw := original_termios
    raw.c_lflag &~= {.ECHO, .ICANON, .ISIG, .IEXTEN}
    raw.c_iflag &~= {.IXON, .ICRNL}
    raw.c_cc[.VMIN] = posix.cc_t(1)
    raw.c_cc[.VTIME] = posix.cc_t(0)
    if posix.tcsetattr(posix.FD(posix.STDIN_FILENO), .TCSAFLUSH, &raw) != .OK {
        fmt.println("Cannot configure terminal input.")
        ok = false
        return
    }
    return
}

term_restore :: proc(original_termios: ^posix.termios) {
    fmt.println("\x1b[<u\x1b[0m\x1b[?25h\x1b[?1049l")
    posix.tcsetattr(posix.FD(posix.STDIN_FILENO), .TCSAFLUSH, original_termios)
}

run :: proc() -> (ok: bool) {
    if posix.isatty(posix.FD(posix.STDIN_FILENO)) == false ||
        posix.isatty(posix.FD(posix.STDOUT_FILENO)) == false {
        fmt.println("An interactive terminal is required.")
        return false
    }

    original_termios := term_prepare() or_return
    defer term_restore(&original_termios)

    fmt.println("\x1b[?1049h\x1b[?25l\x1b[>28u")
    message := ""
    draw_screen(message)
    for {
        key := read_key()
        if key == -1 || key == 27 || key == 'q' || key == 3 {
            break
        }
        if key == 0 {
            continue
        }
        message = action_for_key(key)
        if message != "" {
            draw_screen(message)
        }
    }
    return true
}

main :: proc() {
    code := 0
    ok := run()
    if !ok {
        code = 1
    }
    os.exit(code)
}
