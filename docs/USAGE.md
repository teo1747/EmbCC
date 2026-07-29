# EmbCC / EmbLD — usage

How to invoke the compiler (`embcc`) and the linker (`embld`). This is the CLI
reference; for *what* is implemented and what is still coming, see
[todo.md](todo.md); for the design, [ARCHITECTURE.md](ARCHITECTURE.md).

EmbCC is a self-contained C compiler + linker: `embcc` turns C into ELF objects,
`embld` links them into an ELF executable (or an EMBX binary). No GCC/binutils in
the loop — the pair compiled and linked the EmbLinkOS kernel to a booting desktop.

## `embcc` — the compiler

```
usage: embcc [-E] -c FILE.c [-o FILE.o] [-I DIR]... [flags]
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

## `embld` — the linker

```
usage: embld [-o OUT] [-e ENTRY] [-Ttext ADDR] INPUT.o ...
       embld --embx --cap NAME [--cap NAME]... -o OUT.embx INPUT.o ...
```

| Flag | Meaning |
|------|---------|
| `-o OUT` | Output path. |
| `-e ENTRY` | Entry-point symbol (e.g. `_start`). |
| `-Ttext ADDR` | Base virtual address of `.text` (e.g. the higher-half kernel base). |
| `--embx` | Emit an **EMBX** binary (the OS's native capability-declaring format) instead of a plain ELF. |
| `--cap NAME` | Declare a capability the EMBX binary requires (repeatable). Names match the OS cap set: `filesystem`, `network`, `gpu`, `audio`, `camera`, `usb`, `serial`, `rawdisk`, `kernel_ext`. The kernel enforces the declared set ⊆ the grantor's at load. |

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

## Roadmap-gated invocations (not yet live)

These are tracked in [todo.md](todo.md) and will error today:

- **`embcc --asm FILE.asm`** — a standalone assembler front-end (todo **A1**), to
  drop `nasm` from the build. In progress; not yet accepted.
- **`embld -T SCRIPT.ld`** — linker-script / symbol-assignment support (todo
  **L1**, e.g. `kernel_end = .`), so the kernel links with no external tools and
  no diagnostic stub.

Run `embcc` or `embld` with no arguments to see the current usage line.
