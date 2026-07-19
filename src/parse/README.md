# src/parse

C subset → AST — ../../docs/ARCHITECTURE.md §2.

M1 state: functions of int with int params (≤6), local declarations,
return, + - * with precedence and parens, calls. Recursive descent;
first error is fatal with file:line.
