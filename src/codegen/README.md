# src/codegen

IR → x86-64, System V AMD64 — ../../docs/ARCHITECTURE.md §4.

## The value model

The baseline (`-O0`) is deliberately simple: every vreg lives in a stack slot,
every operation goes through `rax` (and `rcx` for a second operand), so all
values are in memory across statements. Correct and easy to reason about. On top
of that the codegen carries several quality passes, each gated by optimization
level so the lower levels stay reproducible:

- **Stack-slot coalescing (always on).** Temporaries whose live ranges do not
  overlap share one slot (`coalesce_temps`, single-basic-block liveness); local
  variables whose *lexical scopes* are disjoint, or — for non-address-taken
  locals — whose *liveness* is disjoint, share a slot (`coalesce_locals`,
  interval colouring). This keeps frames near GCC's; a stack pointer used past
  its scope is UB, so scope-based sharing is sound even for address-taken arrays.
- **RAX residency cache (`-O1`).** A value just computed into `rax` is not
  reloaded from its slot to be used again; a zero-extend-aware relaxation lets a
  narrow store be reused by a wider read.
- **Register allocation (`-O2`).** Eligible vregs (temps, and scalar
  int/long/pointer/char/short locals, params, and scalar-integer call arguments)
  live in the five **callee-saved** registers (rbx, r12–r15) instead of memory —
  callee-saved so a value survives a call untouched, with no spill-around-call
  logic. Real backward-liveness dataflow builds a precise interference graph
  (interfering within live-in and within live-out, so a dying operand and a
  fresh result can share); Chaitin-Briggs optimistic colouring assigns registers
  with move-coalescing bias and spills the most-constrained node. Any vreg
  touching an opaque raw-slot site (a float op, address-of, atomic, memcpy,
  store-address, va_start, a struct/float call arg, or inline asm — or live
  across an asm) stays in memory.

## Target details

- Integer args in the six SysV registers; return in `rax`/`rdx`. Floats in
  `xmm0–7`. 16-byte stack alignment at every `call`.
- `-mno-sse` (kernel mode): the varargs prologue skips its xmm register-save
  spill and any float op is refused loudly, so nothing #UDs before CR4.OSFXSR is
  set.
- `__attribute__((aligned(N)))` is honoured in layout — struct member offsets,
  struct size/align, and stack-slot alignment (N ≤ 16; larger is refused).
- Intra-unit calls are patched here (rel32 once all functions are placed);
  external call sites, and string/global/function-address sites, become
  relocations the driver hands to the ELF writer.

Inline asm carrying explicit register constraints is assembled in `../ir` (see
its README); its operands reach codegen already bound to fixed registers.
