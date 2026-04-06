CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic -std=c11
LDFLAGS ?=
LDLIBS ?= -lcurl -lsqlite3 -lmosquitto -lpthread -lm

SRC := src/main.c src/util.c src/storage.c src/fetch.c src/psk.c src/http_server.c
OBJ := $(SRC:.c=.o)

TARGET := propagation_bot

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS) $(LDLIBS)

clean:
	rm -f $(OBJ) $(TARGET)
