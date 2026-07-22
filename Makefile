# EmbCC — host build (ROADMAP M0: EmbCC is a host program at this stage).
#
# Plain make on purpose: the eventual on-OS build goes through EmbBuild
# (ROADMAP M4), so nothing here may grow host-only cleverness the manifest
# could not express.
#
# -std=c99: self-hosting constrains the source to the subset EmbCC will
# implement (ARCHITECTURE.md §7). Keep it buildable by a strict C99 compiler.

CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wextra -Werror -g
BUILD   := build

SRCS := \
	src/driver/main.c \
	src/driver/util.c \
	src/lex/lex.c \
	src/parse/parse.c \
	src/sema/sema.c \
	src/sema/type.c \
	src/ir/irgen.c \
	src/codegen/codegen.c \
	src/asm/emit.c \
	src/cpp/predef.c \
	src/elf/write.c

OBJS := $(SRCS:src/%.c=$(BUILD)/%.o)

all: embcc

embcc: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

$(BUILD)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# The tree is small; every object depending on every header is honest
# enough and cannot go stale (CONTRIBUTING lie #1).
$(OBJS): $(wildcard src/*/*.h)

test: embcc
	tests/run.sh

clean:
	rm -rf $(BUILD) embcc

.PHONY: all test clean
