#---------------------------------------------------------------------------------
# Host unit tests for the platform-independent logic in source/plex_api.c.
#
# This is a SEPARATE build from the main devkitARM Makefile at the repo root.
# It compiles the real source/plex_api.c with a native host compiler (not
# devkitARM's arm-none-eabi-gcc, which only produces 3DS binaries that can't
# run here) against two small stand-ins for 3DS-only pieces - see
# tests/stubs/ and the comment at the top of test_plex_api.c.
#
# Usage (from repo root or from tests/):
#   make -f tests/Makefile
#---------------------------------------------------------------------------------
TOPDIR    := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
SRC       := $(TOPDIR)/source
STUBS     := $(TOPDIR)/tests/stubs
BUILD     := $(TOPDIR)/tests/build

CC        := gcc
CFLAGS    := -std=gnu11 -Wall -Wextra -g -I$(STUBS) -I$(SRC) -I$(SRC)/lib
LDFLAGS   := -lcurl

SOURCES   := $(TOPDIR)/tests/test_plex_api.c \
             $(SRC)/plex_api.c \
             $(SRC)/lib/cJSON.c \
             $(STUBS)/apt_stub.c \
             $(STUBS)/logger_stub.c

TARGET    := $(BUILD)/test_plex_api

.PHONY: all test clean

all: test

test: $(TARGET)
	@echo "--- running tests ---"
	@$(TARGET)

$(TARGET): $(SOURCES) | $(BUILD)
	$(CC) $(CFLAGS) $(SOURCES) $(LDFLAGS) -o $@

$(BUILD):
	@mkdir -p $@

clean:
	@rm -rf $(BUILD)
