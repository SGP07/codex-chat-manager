CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=c11
LDLIBS := $(shell pkg-config --libs ncursesw json-c sqlite3 libzstd)
CPPFLAGS := $(shell pkg-config --cflags ncursesw json-c sqlite3 libzstd)

chat-manager: chat-manager.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f chat-manager
.PHONY: clean
