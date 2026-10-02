package main

KEY_SHIFT_SPACE :: 256
KEY_UP          :: 257
KEY_DOWN        :: 258
KEY_RIGHT       :: 259
KEY_LEFT        :: 260

parse_decimal :: proc(sequence: string, index: ^int) -> (value: int, ok: bool) {
    start := index^
    for index^ < len(sequence) && sequence[index^] >= '0' && sequence[index^] <= '9' {
        value = value*10 + int(sequence[index^] - '0')
        index^ += 1
    }
    return value, index^ > start
}

arrow_key :: proc(key: byte) -> int {
    if key >= 'A' && key <= 'D' {
        return KEY_UP + int(key-'A')
    }
    return 0
}

decode_key_sequence :: proc(sequence: string) -> int {
    if len(sequence) == 0 {
        return 0
    }
    if len(sequence) == 1 {
        key := arrow_key(sequence[0])
        if key != 0 {
            return key
        }
        return int(sequence[0])
    }
    if sequence == "27;2;32~" {
        return KEY_SHIFT_SPACE
    }

    index := 0
    code, ok := parse_decimal(sequence, &index)
    if !ok {
        return 0
    }
    shifted := 0
    if index < len(sequence) && sequence[index] == ':' {
        index += 1
        shifted, ok = parse_decimal(sequence, &index)
        if !ok {
            return 0
        }
        if index < len(sequence) && sequence[index] == ':' {
            index += 1
            _, ok = parse_decimal(sequence, &index)
            if !ok {
                return 0
            }
        }
    }

    modifiers := 1
    if index < len(sequence) && sequence[index] == ';' {
        index += 1
        if index < len(sequence) && sequence[index] != ';' && sequence[index] != 'u' {
            modifiers, ok = parse_decimal(sequence, &index)
            if !ok {
                return 0
            }
        }
    }
    event := 1
    if index < len(sequence) && sequence[index] == ':' {
        index += 1
        event, ok = parse_decimal(sequence, &index)
        if !ok {
            return 0
        }
    }
    has_text := index < len(sequence) && sequence[index] == ';'
    produced := 0
    if has_text {
        index += 1
        produced, ok = parse_decimal(sequence, &index)
        if !ok {
            return 0
        }
        if index < len(sequence) && sequence[index] == ':' {
            produced = 0
        }
    }

    if index < len(sequence) && sequence[index] == 'u' {
        if modifiers < 1 || event == 3 {
            return 0
        }
        bits := modifiers - 1
        keys := bits & ~int(192)
        if code == 32 && keys == 1 {
            return KEY_SHIFT_SPACE
        }
        if code == 99 && keys == 4 {
            return 3
        }
        if keys == 0 || keys == 1 {
            if has_text {
                if produced >= 1 && produced <= 127 {
                    return produced
                }
                return 0
            }
            if keys == 1 && shifted >= 1 && shifted <= 127 {
                return shifted
            }
        }
        if code >= 'a' && code <= 'z' && (keys == 0 || keys == 1) {
            if ((bits & 1) != 0) != ((bits & 64) != 0) {
                return code - 'a' + 'A'
            }
            return code
        }
        if keys == 0 && code >= 1 && code <= 127 {
            return code
        }
        return 0
    }

    if code == 1 && index+1 == len(sequence) {
        return arrow_key(sequence[index])
    }
    return 0
}