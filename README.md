# roam

A small Linux terminal file explorer written in C11. It opens the alternate
screen, centers the active directory column, and keeps parent directories visible
to its left when space allows. No external libraries are needed.

Build with `make`, then run `./roam [directory]`. Without a directory argument it
starts in the current working directory. Run it from an interactive terminal.
Set `EDITOR` to a terminal editor (arguments are allowed, e.g. `EDITOR='vim -u NONE'`).

- Up/Down (or `k`/`j`): select an entry
- Right (or `l`): open the selected directory
- Left (or `h`): return to the parent column
- Enter (or `e`): open the selected file or directory with `$EDITOR`
- `n` / `N`: create a file / directory in the current directory
- `r`: rename the selected entry (never overwrites another entry)
- `d`: remove the selected entry after confirmation (directories must be empty)
- `q` or Ctrl-C: quit and restore the previous screen

To change the calling shell's directory, put `roam` on your `PATH` and add this
`cdi` function to your shell configuration:

```sh
cdi() {
	local destination
	destination="$(command roam --cd "$@")" || return
	if [ -n "$destination" ]; then
		builtin cd -- "$destination"
	fi
}
```

In `--cd` mode, press `c` on a selected directory to choose it, or `q` to
cancel. The interface stays on the terminal; only the selected path is written
to stdout for the shell to consume. A program cannot change its parent shell's
working directory directly.

Entries are sorted by name. Right opens only directories; Enter/`e` delegates
file and directory editing to the configured editor. The browser includes hidden
entries other than `.` and `..`. Entry colors
follow `LS_COLORS` when it is set, including file types and filename patterns;
without it, entries use the terminal's default colors.