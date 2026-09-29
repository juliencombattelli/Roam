# Demo tree

Browse with `./roam fixtures/demo-tree`.

The tree includes nested directories, a long directory name, a name containing
a space, a hidden entry, and several file extensions for navigation and color
testing. `run-demo.sh` is executable, `report-link.md` and `alpha-link` are
relative symlinks to a file and directory, and `sample-bundle.tar.gz` contains
`plain.txt` and `alpha/notes.txt`.

`demo-tree/empty` contains a `.gitkeep` file so Git retains it; remove that
placeholder in the explorer to test a truly empty directory.