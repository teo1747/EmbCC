# src/lex

Tokens — ../../docs/ARCHITECTURE.md §2.

The full C99 token set the rest of the compiler needs: identifiers and the
keyword table (including C11 `_Alignof`/`_Alignas`/`_Atomic`/`_Static_assert`/
`_Generic`/`_Bool` and the GNU spellings `typeof`/`__typeof__`, `__asm__`,
`__attribute__`, `__inline__`), integer and floating constants with their
suffixes, character constants and string literals — including the wide and
prefixed forms (`L`, `u`, `U`, `u8`) — the complete operator and punctuator set,
and the `# line` markers the preprocessor emits.

Every token carries file, line and **column**, which is what makes the caret
diagnostics in `../driver/util.c` possible.

Constructs genuinely outside the compiler's reach are rejected here with a
diagnostic naming them, per THE RULE — never lexed into something that means
something else.
