.PHONY: all clean

all: code

code: main.c buddy.c buddy.h utils.h
	gcc -std=c11 -Wall -Wextra -Wno-error=int-conversion -O2 -o $@ main.c buddy.c

clean:
	rm -f code
