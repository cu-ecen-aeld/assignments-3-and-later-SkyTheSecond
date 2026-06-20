# Makefile for aesdsocket
#
# Supports native build:
#   make
#   make all
#
# Supports cross compilation, e.g. for ARM targets used in later
# AESD assignments:
#   make CROSS_COMPILE=aarch64-none-linux-gnu-
#
# CROSS_COMPILE is empty by default (native build). When set, it is
# prefixed to the compiler/tool names as is standard practice for
# kernel-style cross-compilation Makefiles.

CROSS_COMPILE ?=
CC := $(CROSS_COMPILE)gcc

TARGET := aesdsocket
SRCS := aesdsocket.c
OBJS := $(SRCS:.c=.o)

CFLAGS ?= -Wall -Wextra -g -std=gnu99
LDFLAGS ?=

.PHONY: all default clean

default: all

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS)
