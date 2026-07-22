# src/sema

Types, declarations, diagnostics — ../../docs/ARCHITECTURE.md §2.

State: the type system lives here (type.h/type.c — void, the integer
types with unsigned variants, pointers; LP64). Sema annotates every
expression with a type and materializes every implicit C conversion as
an explicit cast node, so irgen never guesses widths. Also: prototype/
definition merging with C linkage rules, lvalue checking, one flat
scope per function (shadowing rejected — strictly fewer programs than
C99), all-paths-return, undefined-static-but-called refused.
