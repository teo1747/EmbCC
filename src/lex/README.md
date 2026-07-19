# src/lex

Tokens — ../../docs/ARCHITECTURE.md §2.

M1 state: the subset tokens (int/void/return/static, integers, + - *,
punctuation). Constructs outside the subset are rejected here with a
diagnostic naming them; keywords the subset lacks lex as identifiers so
the parser can refuse them with better context.
