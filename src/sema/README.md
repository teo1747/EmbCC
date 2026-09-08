# src/sema

Types, declarations, checking, diagnostics — ../../docs/ARCHITECTURE.md §2.

**The type system** lives in `type.h`/`type.c`: void, the integer types with
their unsigned variants, `_Bool`, `float`/`double`, pointers, arrays, functions,
and the tagged types (struct/union/enum) with SysV layout — member offsets,
size, alignment, bitfield packing, and `__attribute__((aligned(N)))`. LP64.

Sema annotates every expression with a type and **materializes every implicit C
conversion as an explicit cast node**, so irgen never has to guess a width. That
one decision is why the integer-promotion and usual-arithmetic-conversion rules
live in exactly one place.

Also here: nested block scopes with shadowing, ordinary/tag/label namespaces,
prototype/definition merging under C linkage rules, `static`/`extern` (file and
block scope) and tentative definitions, lvalue and assignability checking,
constant-expression evaluation for initializers, enum and case values,
`_Static_assert` and `_Generic` resolution, all-paths-return, and
volatile-qualification tracked through to the IR so the optimizer preserves
every MMIO access.

Diagnostics carry the column of the offending node, which is what gives semantic
errors a caret and not just a line number.
