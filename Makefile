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
	src/as/as.c \
	src/opt/opt.c \
	src/codegen/codegen.c \
	src/debug/dwarf.c \
	src/asm/emit.c \
	src/asm/topasm.c \
	src/cpp/predef.c \
	src/cpp/cpp.c \
	src/elf/write.c

OBJS := $(SRCS:src/%.c=$(BUILD)/%.o)

all: embcc embread embld embas

embcc: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

# embas — the standalone NASM/Intel-syntax assembler (A1, ARCHITECTURE §4). Reads
# the kernel's hand-written .asm and emits ELF objects the same writer (src/elf)
# the compiler uses produces, so the toolchain owns the whole build (drops nasm).
embas: tools/embas/embas.c src/as/as.c src/as/as.h src/elf/write.c \
       src/elf/elf.h src/driver/util.c
	$(CC) $(CFLAGS) -o $@ tools/embas/embas.c src/as/as.c src/elf/write.c \
	    src/driver/util.c

# embld — the integrated linker (ARCHITECTURE §6, WORKPLAN stream B), as
# a standalone tool for host development. The link library also gets
# wired into embcc so `embcc prog.c -o prog` links in-process.
# embld also links the EmbDBG core (compiled -DEMBDBG_NO_MAIN, so no CLI main)
# so the linker can emit a native .embdbg at link time through the SAME format
# writer the embdbg tool uses — one implementation, not two.
embld: tools/embld/embld.c src/link/link.c src/driver/util.c \
       src/link/link.h src/elf/elf.h src/embx/embx.c src/embx/embx.h \
       tools/embdbg/embdbg.c tools/embdbg/embdbg_core.h
	$(CC) $(CFLAGS) -DEMBDBG_NO_MAIN -Wno-unused-function -o $@ \
	    tools/embld/embld.c src/link/link.c \
	    src/driver/util.c src/embx/embx.c tools/embdbg/embdbg.c

# embread — the EMBX dumper/verifier (EMBX spec §9). A separate binary,
# not part of embcc: it reads images, it does not compile. The EMBX
# container definition it shares with the future linker lives in
# src/embx/, exactly as src/elf/ is shared by asm and link.
embread: tools/embread/embread.c src/embx/embx.c src/embx/embx.h
	$(CC) $(CFLAGS) -o $@ tools/embread/embread.c src/embx/embx.c

# embdbg — EmbDBG v0, the debug-info reader/symbolizer (EMBDBG step 1's
# consumer). Standalone like embread: it reads the DWARF EmbCC emits, it does
# not compile. The live-control half is gated on the kernel debug contract.
embdbg: tools/embdbg/embdbg.c src/elf/elf.h
	$(CC) $(CFLAGS) -o $@ tools/embdbg/embdbg.c

$(BUILD)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# The tree is small; every object depending on every header is honest
# enough and cannot go stale (CONTRIBUTING lie #1).
$(OBJS): $(wildcard src/*/*.h)

test: embcc embread embld embdbg
	tests/run.sh

clean:
	rm -rf $(BUILD) embcc embread embld embdbg embas

.PHONY: all test clean
