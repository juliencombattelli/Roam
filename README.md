# cdi

A small Linux terminal directory browser written in C11. It opens the alternate
screen, centers the active directory column, and keeps parent directories visible
to its left when space allows. No external libraries are needed.

Build with `make`, then run `./cdi [directory]`. Without a directory argument it
starts in the current working directory. Run it from an interactive terminal.

- Up/Down (or `k`/`j`): select an entry
- Right (or `l`): open the selected directory
- Left (or `h`): return to the parent column
- `q` or Ctrl-C: quit and restore the previous screen

Entries are sorted by name. Files can be selected, but only directories can be
opened. The browser includes hidden entries other than `.` and `..`. Entry colors
follow `LS_COLORS` when it is set, including file types and filename patterns;
without it, entries use the terminal's default colors.