# src/sema

Types, declarations, diagnostics — ../../docs/ARCHITECTURE.md §2.

State: name resolution (one flat scope per function, shadowing rejected
so the subset stays strictly inside C99), prototype/definition merging
with C linkage rules (static-then-non-static keeps internal; the reverse
is an error, as in gcc), declaration-before-use, arity, all-paths-return
analysis, and undefined-static-but-called refused at compile time.
