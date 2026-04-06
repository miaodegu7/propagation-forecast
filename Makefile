CC ?= gcc
PKG_CONFIG ?= pkg-config
PKG_DEPS ?= libcurl sqlite3 libmosquitto
THREAD_FLAGS ?= -pthread
EXEEXT ?=

CFLAGS ?= -O2 -Wall -Wextra -Wpedantic -std=c11
LDFLAGS ?=

SRC := src/main.c src/util.c src/storage.c src/fetch.c src/runtime.c src/psk.c src/http_server.c
OBJ := $(SRC:.c=.o)

TARGET ?= propagation_bot$(EXEEXT)

PKG_CONFIG_MODE := $(if $(filter 1,$(STATIC)),--static,)
PKG_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(PKG_DEPS) 2>/dev/null)
PKG_LIBS := $(shell $(PKG_CONFIG) --libs $(PKG_CONFIG_MODE) $(PKG_DEPS) 2>/dev/null)

ifeq ($(strip $(PKG_LIBS)),)
LDLIBS ?= -lcurl -lsqlite3 -lmosquitto -lm
else
LDLIBS ?= $(PKG_LIBS) -lm
endif

ifeq ($(OS),Windows_NT)
SOCKET_LIBS ?= -lws2_32
else
SOCKET_LIBS ?=
endif

ifeq ($(STATIC),1)
LDFLAGS += -static -static-libgcc
endif

CFLAGS += $(PKG_CFLAGS) $(THREAD_FLAGS)
LDLIBS += $(THREAD_FLAGS) $(SOCKET_LIBS)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS) $(LDLIBS)

clean:
	rm -f $(OBJ) $(TARGET)
