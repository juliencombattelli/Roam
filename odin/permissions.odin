package main

permission_color :: proc(permission: rune, file_type: bool, entry_color, executable_color: string) -> string {
    if file_type && permission == '.' {
        return "37"
    }
    if file_type || permission == 's' || permission == 'S' || permission == 't' || permission == 'T' {
        return entry_color
    }
    if permission == 'r' {
        return "33"
    }
    if permission == 'w' {
        return "31"
    }
    if permission == 'x' {
        return executable_color
    }
    return ""
}