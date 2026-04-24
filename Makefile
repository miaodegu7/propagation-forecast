CC ?= gcc
CXX ?= g++
PKG_CONFIG ?= pkg-config
PKG_DEPS ?= libcurl sqlite3 libmosquitto
THREAD_FLAGS ?= -pthread
EXEEXT ?=

CFLAGS ?= -O2 -Wall -Wextra -Wpedantic -std=c11
CXXFLAGS ?= -O2 -Wall -Wextra -std=c++17 -DUNICODE -D_UNICODE
LDFLAGS ?=

SRC := src/main.c src/util.c src/storage.c src/fetch.c src/runtime.c src/psk.c src/hamalert.c src/awards.c src/http_server.c
OBJ := $(SRC:.c=.o)
DESKTOP_SRC := src/desktop_win.cpp
DESKTOP_OBJ := $(DESKTOP_SRC:.cpp=.o)

TARGET ?= propagation_bot$(EXEEXT)
DESKTOP_TARGET ?= propagation_desktop$(EXEEXT)

PKG_CONFIG_MODE := $(if $(filter 1,$(STATIC)),--static,)
PKG_CFLAGS := $(shell $(PKG_CONFIG) $(PKG_CONFIG_MODE) --cflags $(PKG_DEPS) 2>/dev/null)
PKG_LIBS := $(shell $(PKG_CONFIG) --libs $(PKG_CONFIG_MODE) $(PKG_DEPS) 2>/dev/null)

ifeq ($(strip $(PKG_LIBS)),)
LDLIBS ?= -lcurl -lsqlite3 -lmosquitto -lm
else
LDLIBS ?= $(PKG_LIBS) -lm
endif

ifeq ($(OS),Windows_NT)
SOCKET_LIBS ?= -lws2_32 -lole32 -lgdiplus
DESKTOP_LDLIBS ?= -lwinhttp -lshell32 -luser32 -lgdi32
DESKTOP_LDFLAGS ?= -mwindows -municode -static-libgcc -static-libstdc++
TARGETS := $(TARGET) $(DESKTOP_TARGET)
else
SOCKET_LIBS ?=
TARGETS := $(TARGET)
endif

ifeq ($(STATIC),1)
LDFLAGS += -static -static-libgcc
endif

CFLAGS += $(PKG_CFLAGS) $(THREAD_FLAGS)
LDLIBS += $(THREAD_FLAGS) $(SOCKET_LIBS)

.PHONY: all clean

all: $(TARGETS)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS) $(LDLIBS)

$(DESKTOP_TARGET): $(DESKTOP_OBJ)
	$(CXX) $(CXXFLAGS) $(DESKTOP_LDFLAGS) -o $@ $(DESKTOP_OBJ) $(DESKTOP_LDLIBS)

clean:
	rm -f $(OBJ) $(DESKTOP_OBJ) $(TARGET) $(DESKTOP_TARGET) propagation_bot.exe propagation_desktop.exe
