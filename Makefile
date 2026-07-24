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
	src/cpp/cpp.c \
	src/elf/write.c

OBJS := $(SRCS:src/%.c=$(BUILD)/%.o)

all: embcc embread embld

embcc: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

# embld — the integrated linker (ARCHITECTURE §6, WORKPLAN stream B), as
# a standalone tool for host development. The link library also gets
# wired into embcc so `embcc prog.c -o prog` links in-process.
embld: tools/embld/embld.c src/link/link.c src/driver/util.c \
       src/link/link.h src/elf/elf.h
	$(CC) $(CFLAGS) -o $@ tools/embld/embld.c src/link/link.c \
	    src/driver/util.c

# embread — the EMBX dumper/verifier (EMBX spec §9). A separate binary,
# not part of embcc: it reads images, it does not compile. The EMBX
# container definition it shares with the future linker lives in
# src/embx/, exactly as src/elf/ is shared by asm and link.
embread: tools/embread/embread.c src/embx/embx.c src/embx/embx.h
	$(CC) $(CFLAGS) -o $@ tools/embread/embread.c src/embx/embx.c

$(BUILD)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# The tree is small; every object depending on every header is honest
# enough and cannot go stale (CONTRIBUTING lie #1).
$(OBJS): $(wildcard src/*/*.h)

test: embcc embread embld
	tests/run.sh

clean:
	rm -rf $(BUILD) embcc embread embld

.PHONY: all test clean
