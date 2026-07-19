# src/parse

C subset → AST — ../../docs/ARCHITECTURE.md §2.

State: functions of int with int params (≤6), declarations, return,
if/else, while, for, blocks, assignment, full binary precedence chain
(|| && == rel + *) with parens and !. Recursive descent; first error
is fatal with file:line; reserved C words it cannot handle are named
in the diagnostic.
