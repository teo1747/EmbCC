# src/sema

Types, declarations, diagnostics — ../../docs/ARCHITECTURE.md §2.

State: name resolution (one flat scope per function, shadowing
rejected so the subset stays strictly inside C99), definition-before-
use, arity, and conservative all-paths-return analysis — a maybe-
missing return is refused, never miscompiled.
