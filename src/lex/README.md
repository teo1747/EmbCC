# src/lex

Tokens — ../../docs/ARCHITECTURE.md §2.

State: subset tokens incl. comparisons (== != < <= > >=), && || !,
if/else/while/for keywords. Constructs outside the subset are rejected
here with a diagnostic naming them; keywords the subset lacks lex as
identifiers so the parser can refuse them with better context.
