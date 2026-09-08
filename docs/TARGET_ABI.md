# The EmbLinkOS target contract

*What EmbCC must emit for a program to load and run on EmbLinkOS. This is the
**grounding document**: every fact here was established empirically against the
real kernel loader (`kernel/arch/x86_64/syscall/elf.c`) and the real toolchain,
mostly while teaching TCC to build dynamic GUI apps. Where a fact was learned by
something breaking, the breakage is recorded — those are the expensive facts.*

**Target triple in spirit:** `x86_64-elf`, System V AMD64 calling convention,
LP64, no PIE, no `ld.so`.

**Two targets, present and future.** Everything in this document describes the
**current** target EmbCC emits for: an **ELF** executable linked against
**newlib**, which is what runs on EmbLinkOS today and is the substrate for
porting foreign source (git/CPython/C++). That target is not going away — it is
how the OS meets the existing software world.

The **native** target — reached, and emitted by the toolchain today (DECISIONS
D-003 revised, D-009; `embld --embx --cap NAME`) — is:
- **format:** **EMBX** — `myos/docs/EMBX_Specification_v2.md`, byte-exact, with
  a working in-kernel loader. Same SysV/LP64/no-PIE code inside; a different,
  capability-carrying container around it. An EMBX APP is fully linked (no
  relocations), so the whole dynamic-linking §4 complexity below does **not**
  apply to it. The one addition is the **capability table** (spec §5): the
  binary declares the resource classes it needs, checked at load against the
  spawning process's set.
- **libc:** **emlibc** — `myos/docs/EMLIBC_Requirements.md`, non-POSIX,
  EmbLink-shaped. The crt0 contract (§2) and the syscall convention (§3) are
  the parts emlibc keeps from what is documented here; the POSIX-costume parts
  fall away.

Read the rest of this document as the **current** contract, which is also the
foundation the native target is derived from — the SysV ABI, the crt0 entry,
and the `int 0x80` convention are shared by both. EMBX and emlibc change the
*container* and the *library shape*, not the instruction-level contract.

---

## 1. The rule that shapes everything: the kernel *is* the linker

EmbLinkOS has **no userspace dynamic loader**. There is no `ld.so`, and there is
no `exec` for one to be loaded by. The kernel's ELF loader reads the program,
and — if it is dynamic — loads the one shared object itself and performs the
symbol binding in-kernel.

Consequences EmbCC must respect:

- A `PT_INTERP` segment is **harmless but pointless**: the loader only maps
  `PT_LOAD` segments and never reads the interpreter path. Emitting it is not an
  error; emitting a program that *depends* on it is.
- There is **no lazy binding**. All relocations are applied eagerly at load.
- There is **no runtime libc.so**. newlib is linked *statically into every
  executable*. This single fact drives most of §4.

## 2. Process entry

The kernel loads segments, then jumps to `e_entry` in ring 3 with a fresh stack.
The entry symbol is **`_start`**, provided by `crt0.o` (part of the ABI, see §5),
and its contract is:

```c
void _start(int argc, char **argv, char **envp);
```

- `argc`/`argv`/`envp` arrive **on the stack per the spawn** — the environment is
  passed *explicitly at spawn time and never inherited* (`envp == NULL` legally
  means "no environment").
- `crt0` then seeds `environ`, sets up TLS if present (§6), walks `.init_array`
  and `.ctors`, calls `main`, and calls `exit`.

EmbCC does not need to generate `_start`; it needs to **not conflict with it**
and to leave `main` as an ordinary global function.

## 3. System calls

Syscalls are made with **`int $0x80`** (not `syscall`/`sysenter`). The
convention, from `user/lib/embk_syscall.h`:

| Register | Meaning |
|---|---|
| `rax` | syscall number (in, and the return value out) |
| `rdi` | arg 1 |
| `rsi` | arg 2 |
| `rdx` | arg 3 |
| `r10` | arg 4 |
| `r8`  | arg 5 |
| `r9`  | arg 6 |

Clobbered: `rcx`, `r11`, memory. Return convention is Linux-style: a value in
`[-4095, -1]` is `-errno`; anything else is a real result.

> **Expensive fact.** The ABI header's inline-asm bindings originally used
> gcc's `register ... __asm__("r10")` form. TCC ignores those bindings and dies
> allocating the resulting generic `"r"` constraints ("asm constraint 6 could not
> be satisfied"), so the header now carries a `__TINYC__` branch passing the high
> arguments through memory. **A compiler that cannot express fixed-register
> operands must provide its own path here.** EmbCC should support explicit
> register constraints natively and avoid inheriting this workaround.

## 4. The two link shapes

### 4a. Static executable (tools, the shell, EmbBuild)

```
ET_EXEC, no PT_DYNAMIC. crt0.o + syscalls.o + objects + libc.a
```

Everything resolves at link time. This is what most of the userland is.

> **Expensive fact (TCC patch 0001).** In a static link, `R_X86_64_PLT32` must
> be treated as a plain `PC32` call. A PLT slot exists so a *shared object* can
> preempt a call; a static link has no resolver, so any PLT it mints is
> unbindable. TCC emitted PLT stubs for **407 ordinary calls** and the program
> died at a wild jump — silently, with a valid-looking ELF. **EmbCC must not
> route calls through a PLT in a static link.**

> **Expensive fact (TCC patch 0003).** In a static link the **GOT must still be
> relocated**. TCC's relocation walk skipped `.got` (correct only when a dynamic
> loader will fill it), leaving every `GOTPCREL` slot zero — and newlib reaches
> `stderr`/`errno` through `_impure_ptr`, a GOTPCREL access. Result: `CR2=0` on
> the first stdio or errno touch. **If EmbCC emits GOT-indirect data access, it
> must fill the GOT itself when static.**

### 4b. Dynamic executable (EmUI/GUI apps against `libembk.so`)

```
ET_EXEC (never ET_DYN/PIE) + PT_DYNAMIC
crt0.o + syscalls.o + objects + libembk.so + libc.a + libm.a  (+ intrinsics)
```

The kernel loads `/system/lib/libembk.so` — **hardwired**; the app's `DT_NEEDED`
is not consulted — at a fixed bias (`DYLIB_VA_BASE`) and performs a **two-way
bind**:

- the app's imports resolve to `libembk.so`'s exports, **and**
- `libembk.so`'s imports (all libc/libm: `memcpy`, `vsnprintf`, `cosf`, …)
  resolve back to **the app's statically-linked, re-exported newlib**.

Hard requirements for the app image:

| Requirement | Why |
|---|---|
| `e_type == ET_EXEC` | The loader rejects anything else. No PIE. |
| `PT_DYNAMIC` present | It is what marks the program as needing the bind |
| **classic `DT_HASH`** | The loader reads `hash[1]` (`nchain`) as the dynsym count. **`DT_GNU_HASH` alone will not work** (`--hash-style=sysv`) |
| `DT_SYMTAB`, `DT_STRTAB` | Symbol resolution is by name, linear |
| `DT_RELA`/`DT_RELASZ`, `DT_JMPREL`/`DT_PLTRELSZ` | The two relocation tables applied |
| App **exports** its newlib | `--export-dynamic` / `-rdynamic`, else `libembk.so`'s imports cannot bind |

**Relocation types the loader implements — and only these:**

`R_X86_64_RELATIVE`, `R_X86_64_COPY`, `R_X86_64_64`, `R_X86_64_GLOB_DAT`,
`R_X86_64_JUMP_SLOT`. Anything else is a hard `ENOEXEC` ("unhandled reloc
type"). EmbCC must confine dynamic output to this set.

> **Expensive fact (TCC patch 0004).** Because there is no runtime `libc.so`, a
> shared library's *undefined* libc symbols must be **pulled into the executable**
> from a following archive and re-exported. GNU ld does this naturally with
> `libembk.so -lc --export-dynamic`; TCC only recorded them in `dynsymtab` and
> warned, so apps shipped without `vsnprintf`/`cosf` and the loader failed to
> resolve them for the library. **EmbCC's linker must satisfy a shared object's
> undefined symbols from later archives.**

## 5. The ABI on disk (`/system/abi`)

The sealed contract every EmbLink program targets:

| File | What it is |
|---|---|
| `crt0.o` | `_start`, environ/TLS/ctors setup, calls `main`, exits |
| `syscalls.o` | the newlib retargeting layer (the `int $0x80` bindings) |
| `libc.a` | newlib — *the same archive the cross build uses*, so a program built **on** the OS and one built **for** it are the same program |
| `libm.a` | math (`cosf`/`sinf` — `libembk.so` imports these) |
| `include/` | newlib's headers — declaring the libc is part of the contract |
| `libtcc1.o` | see §7 |
| `emlink_dynstubs.o` | see §6 |

## 6. Weak symbols, and a loader subtlety worth knowing

`crt0` references six **weak** bracket symbols — `__ctors_start`, `__ctors_end`,
and the TLS geometry `__tls_image`, `__tls_filesz`, `__tls_memsz`, `__tls_align`.
In the gcc build the linker script (`newlib.ld`) `PROVIDE`s them; with no linker
script they are simply undefined, and **standard ELF semantics say a weak
undefined symbol binds to 0**.

> **Expensive fact.** TCC does not bind weak-undefined to 0 in a dynamic link —
> it defers them to `GLOB_DAT` relocations. Two things then bite:
> 1. Nothing defines them, so the loader reports `UNRESOLVED symbol
>    '__tls_memsz'`. Mitigated with `emlink_dynstubs.o`, which defines all six as
>    **weak absolute 0** — the tcc-world equivalent of `newlib.ld`'s `PROVIDE`s.
> 2. Even when *defined as absolute 0*, the loader's `resolve_sym()` returns
>    `bias + st_value == 0`, and its caller treats a return of **0 as "not
>    found."** A symbol that legitimately resolves to address 0 is
>    indistinguishable from a missing one.
>
> **For EmbCC this is a design input, not just trivia:** bind weak-undefined
> symbols to 0 **at link time** (as GNU ld does) and emit no relocation for
> them. Then neither problem can occur. (The loader-side fix — distinguish
> found-ness from value — belongs to the OS and is tracked there.)

## 7. Compiler-runtime intrinsics

x86-64 `libgcc` does **not** contain some helpers a weaker codegen will call —
notably `__floatundisf` (unsigned 64-bit → float), because gcc *inlines* that
conversion. TCC calls it, so the OS ships `libtcc1.o` (cross-built from TCC's
`lib/libtcc1.c`) providing `__floatundisf`, `__fixunssfdi`, `__udivdi3`,
`__moddi3`, and friends.

**EmbCC will need its own equivalent** the moment its codegen emits a libcall
gcc inlines. Two honest options: inline the conversions in codegen (better), or
ship an `libembcc1` runtime (simpler). Decide before the first float lands.

## 8. Minimum viable output — the M1 checklist

For the first milestone, EmbCC must emit an object that links (with the existing
toolchain, initially) into a program that:

- [ ] is `ET_EXEC`, machine `EM_X86_64`, ELFCLASS64
- [ ] has `_start` reachable from `crt0.o` and defines `main`
- [ ] uses `int $0x80` with the §3 register convention for any syscall
- [ ] emits **no PLT** in a static link, and fills its own **GOT** if it uses one
- [ ] binds weak-undefined symbols to 0 with no relocation
- [ ] confines any relocations to the §4b set
- [ ] **runs on the OS and exits 42**

That last line is the only one that counts. See ROADMAP M1.

## 9. Where to verify these claims

- Loader: `kernel/arch/x86_64/syscall/elf.c` — `elf_load`, `dynamic_link`,
  `apply_relocs`, `resolve_sym`, `parse_dynamic`
- ABI/entry: `user/lib/crt0.c`, `user/lib/syscalls.c`, `user/lib/embk_syscall.h`
- Link recipes: the EmbLinkOS `Makefile` (`NEWLIB_LDFLAGS`, `NEWLIB_DYN_LDFLAGS`,
  `NEWLIB_DYN_WL`)
- The four TCC patches: `tools/tcc/000{1,2,3,4}-*.patch` — each is a fact about
  this target that a compiler author will otherwise learn the hard way
- On-OS proof tests: `test tcc real`, `test tcc tally`, `test embbuild`,
  `test tcc dyn`
