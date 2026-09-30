CC = cc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -O2 -fsanitize=address,undefined
prefix ?= $(HOME)/.local
bindir ?= $(prefix)/bin
INSTALL ?= install

roam: roam.c
	$(CC) $(CFLAGS) -o $@ $<

.PHONY: install clean
install: roam
	$(INSTALL) -d "$(DESTDIR)$(bindir)"
	$(INSTALL) -m 755 roam "$(DESTDIR)$(bindir)/roam"

clean:
	rm -f roam