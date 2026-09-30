CC = cc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -O2 -fsanitize=address,undefined

roam: roam.c
	$(CC) $(CFLAGS) -o $@ $<

.PHONY: clean
clean:
	rm -f roam