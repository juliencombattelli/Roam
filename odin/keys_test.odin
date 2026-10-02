package main

import "core:testing"

@(test)
test_decode_key_sequence :: proc(t: ^testing.T) {
    Key_Case :: struct {
        sequence: string,
        expected: int,
    }
    cases := [?]Key_Case{
        {"A", KEY_UP}, {"B", KEY_DOWN}, {"C", KEY_RIGHT}, {"D", KEY_LEFT},
        {"27;2;32~", KEY_SHIFT_SPACE}, {"32;2u", KEY_SHIFT_SPACE},
        {"97:65;2u", 'A'}, {"110;1;110u", 'n'}, {"99;5u", 3},
        {"47;2u", 0}, {"44;2;233u", 0}, {"44;2:3;63u", 0},
        {"44:60;2u", '<'}, {"44;2;63u", '?'}, {"44:63;2;63u", '?'},
        {"0;;63u", '?'}, {"47;5u", 0}, {"1A", KEY_UP},
    }
    for test_case in cases {
        testing.expect_value(t, decode_key_sequence(test_case.sequence), test_case.expected)
    }
}

@(test)
test_action_for_key :: proc(t: ^testing.T) {
    testing.expect_value(t, action_for_key('n'), "Create file (stub)")
    testing.expect_value(t, action_for_key('N'), "Create directory (stub)")
    testing.expect_value(t, action_for_key('r'), "Rename selected entry (stub)")
    testing.expect_value(t, action_for_key('d'), "Remove selected entry (stub)")
    testing.expect_value(t, action_for_key('?'), "Show key help (stub)")
    testing.expect_value(t, action_for_key('q'), "")
}