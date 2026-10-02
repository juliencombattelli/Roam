package main

import "core:testing"

@(test)
test_permission_color :: proc(t: ^testing.T) {
    testing.expect_value(t, permission_color('.', true, "01;34", "01;32"), "37")
    testing.expect_value(t, permission_color('d', true, "01;34", "01;32"), "01;34")
    testing.expect_value(t, permission_color('r', false, "", "01;32"), "33")
    testing.expect_value(t, permission_color('w', false, "", "01;32"), "31")
    testing.expect_value(t, permission_color('x', false, "", "01;32"), "01;32")
    for permission in "sStT" {
        testing.expect_value(t, permission_color(rune(permission), false, "01;34", "01;32"), "01;34")
    }
    testing.expect_value(t, permission_color('-', false, "", "01;32"), "")
    testing.expect_value(t, permission_color('x', false, "", ""), "")
}