CC = cc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -O2

roam: roam.c
	$(CC) $(CFLAGS) -o $@ $<

.PHONY: clean
clean:
	rm -f roam