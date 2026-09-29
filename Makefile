CC = cc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -O2

cdi: cdi.c
	$(CC) $(CFLAGS) -o $@ $<

.PHONY: clean
clean:
	rm -f cdi