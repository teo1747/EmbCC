# EmbCC / EmbLD — usage

How to invoke the compiler (`embcc`) and the linker (`embld`). This is the CLI
reference; for *what* is implemented and what is still coming, see
[todo.md](todo.md); for the design, [ARCHITECTURE.md](ARCHITECTURE.md).

EmbCC is a self-contained C compiler + linker: `embcc` turns C into ELF objects,
`embld` links them into an ELF executable (or an EMBX binary). No GCC/binutils in
the loop — the pair compiled and linked the EmbLinkOS kernel to a booting desktop.

## `embcc` — the compiler

```
usage: embcc [-E] -c FILE.c [-o FILE.o] [--target=TRIPLE] [-I DIR]... [flags]
       embcc --version | --dump-predef | --emit-empty-object FILE
```

**Modes**

| Flag | Meaning |
|------|---------|
| `-c FILE.c` | Compile one translation unit to an ELF object. |
| `-E` | Preprocess only — write the expanded source to stdout. |
| `--version` | Print the version and exit. |
| `--dump-predef` | Print the built-in predefined macros (what `-E` starts from). |
| `--emit-empty-object FILE` | Write a valid empty ELF object (for empty TUs / build plumbing). |

**Options**

| Flag | Meaning |
|------|---------|
| `--target=TRIPLE` | Which machine to emit for: `x86_64-elf` (default) or `aarch64-elf`. Fixed for the whole compile — it selects the backend, the predefined-macro set, `e_machine` and the relocation types together (D-011). Also accepted before `--version`/`--dump-predef`, which then describe that target. |
| `-o FILE.o` | Output path (default: the input with `.o`). |
| `-I DIR` | Add a header search directory (repeatable). |
| `-isystem DIR` | Add a *system* header search directory (repeatable). |
| `-g` | Emit debug info (DWARF). |
| `-O0` / `-O1` / `-O2` | Optimization level. Bare `-O` = `-O1`. `-O2` enables register allocation. |

**Codegen flags** (for freestanding / kernel targets)

| Flag | Meaning |
|------|---------|
| `-mno-sse` / `-mno-sse2` | No SSE/SSE2 codegen (kernel runs SSE-off until `fpu_init`). |
| `-mno-mmx` | No MMX. |
| `-mno-80387` | No x87 FPU. |
| `-mno-red-zone` | No red zone (required for kernel / interrupt-taking code). |
| `-mgeneral-regs-only` | Integer registers only (implies the above). |

### Examples

```sh
# a normal object
embcc -c hello.c -o hello.o

# preprocess only
embcc -E hello.c

# kernel-grade: freestanding, SSE-off, no red zone, with include roots
embcc -c kernel/mm/pmm.c -Ikernel -mno-sse -mno-sse2 -mno-red-zone -O2 -o pmm.o
```

**Compiling for aarch64**

```sh
# an object for EmbLinkOS's second architecture
embcc --target=aarch64-elf -c prog.c -o prog.o

# what the target's headers say it is
embcc --target=aarch64-elf --dump-predef | grep __aarch64__
```

The result is a real `EM_AARCH64` ET_REL object that `aarch64-elf-ld` links
against stock newlib. `embld` does not read or write aarch64 objects yet, and
`embas` assembles x86-64 NASM syntax only — so an aarch64 link goes through
binutils for now. What the aarch64 backend refuses (inline asm, `va_start`,
atomics, HFA arguments, `-g`) it refuses with a diagnostic naming the gap; the
README's "Where aarch64 stands" table says why each is real work.

To run what it produced, `make test-arm64` links each test into a bare-metal
image and executes it under `qemu-system-aarch64 -M virt` — see
`tests/harness/README.md`.

## `embld` — the linker

```
usage: embld [-o OUT] [-e ENTRY] [-Ttext ADDR] [--lma-offset N] INPUT.o|INPUT.a ...
       embld --embx --cap NAME [--cap NAME]... -o OUT.embx INPUT.o ...
```

| Flag | Meaning |
|------|---------|
| `-o OUT` | Output path. |
| `-e ENTRY` | Entry-point symbol (e.g. `_start`). |
| `-Ttext ADDR` | Base virtual address of `.text` (e.g. the higher-half kernel base). |
| `--lma-offset N` | Subtract `N` from each segment's vaddr to get its load address (`p_paddr`) — how a higher-half kernel is loaded low and run high. |
| `--embx` | Emit an **EMBX** binary (the OS's native capability-declaring format) instead of a plain ELF. |
| `--cap NAME` | Declare a capability the EMBX binary requires (repeatable). Names match the OS cap set: `filesystem`, `network`, `gpu`, `audio`, `camera`, `usb`, `serial`, `rawdisk`, `kernel_ext`. The kernel enforces the declared set ⊆ the grantor's at load. |

**Linker-defined symbols** are provided automatically when referenced and
otherwise undefined, so no linker script is needed: `kernel_end`, `_end`, `end`,
`__bss_end`, `__kernel_end` (the vaddr past the last `.bss` byte), and the
`__init_array_start`/`_end`, `__fini_array_*`, `__ctors_*`, `__dtors_*` bracket
family. A real definition always wins.

### Examples

```sh
# link a freestanding ELF (e.g. the kernel) at the higher-half base
embld -e _start -Ttext 0xFFFFFFFF80100000 -o kernel.elf *.o

# link a plain executable
embld -o app app.o libfoo.a

# emit an EMBX binary that declares it needs the filesystem capability
embld --embx --cap filesystem -o app.embx crt0.o app.o libc.a
```

## Building for EmbLinkOS

EmbCC is the toolchain for the OS. A typical app is compiled against the sealed
ABI and linked into an ELF (or EMBX):

```sh
embcc -c app.c -I/path/to/abi/include -o app.o
embld -o app.elf crt0.o app.o libc.a           # or: --embx --cap filesystem -o app.embx
```

The OS side of this — where the ABI lives (`/system/abi`), how apps declare their
namespace, and how the build is driven — is documented in the OS repo:
`myos/docs/TOOLCHAIN.md` (building for/on the OS) and `myos/docs/BUILD.md`.

## `embas` — the assembler

```
usage: embas [-f elf64] [-o OUT] INPUT.asm
```

NASM/Intel syntax. The same code runs when you hand `embcc -c` a `.asm` file, so
either entry point works:

```sh
embas -f elf64 boot.asm -o boot.o
embcc -c boot.asm -o boot.o          # equivalent
```

Its correctness bar is byte-identity with `nasm -f elf64`, met on all six of the
kernel's hand-written `.asm`. `-f bin` (flat binary, for the 16/32-bit boot
stages) is **not** implemented and reports an error rather than miscompiling.

## `embdbg` — reading the debug info back

Compile with `-g`, then:

```sh
embcc -c foo.c -g -o foo.o
embld -o foo.elf crt0.o foo.o libc.a
embdbg foo.elf funcs                 # list functions + ranges
embdbg foo.elf line 0x401234         # address -> func:file:line
```

`embdbg FILE` with no subcommand prints the full command list (symbolize,
inspect locals, disassemble, and a small TUI). It reads the DWARF-4 EmbCC emits
— no gdb in the loop. See [EMBDBG_Requirements.md](EMBDBG_Requirements.md).

## What is not there

Refused loudly rather than faked, per THE RULE:

- **`embas -f bin`** — flat-binary output; the boot stages still use nasm.
- **`embld -T SCRIPT.ld`** — full linker scripts. Not needed so far: the
  end-of-image and bracket symbols above are auto-provided, which is what let
  the kernel link without one.
- **VLAs, `_Complex`, 80-bit `long double`** — the remaining C gaps, ranked
  against a real corpus in [todo.md](todo.md).
- **C++, `__thread`/TLS, PIE/PIC output** — out of scope by decision
  (ARCHITECTURE §8, DECISIONS D-008).

Run `embcc`, `embas` or `embld` with no arguments for the current usage line.
