CC      ?= gcc
CFLAGS  ?= -O2 -Wall

all: greet

greet: greet.c
	$(CC) $(CFLAGS) -o $@ $<

run: greet
	./greet

clean:
	rm -f greet

.PHONY: all run clean
