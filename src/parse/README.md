# src/parse

C → AST — ../../docs/ARCHITECTURE.md §2.

Recursive descent over the C99 grammar plus the GNU extensions the kernel and
newlib actually use.

**Declarations:** the full declarator grammar — pointers, arrays, functions,
and the awkward nestings (`int (*p)[N]`, `void (*arr[])(void)`); storage classes
and qualifiers; `typedef`; `struct`/`union`/`enum` with bitfields and anonymous
members; initializers including designated (both `.field` and `[i]`, and GNU
array ranges `[a ... b]`) and compound literals.

**Statements:** the whole set — `if`/`else`, `while`, `do`, `for`, `switch`/
`case`/`default`, `break`, `continue`, `goto` and labels, GNU computed `goto`
(`&&label` / `goto *p`), `return`, blocks.

**Expressions:** the full precedence chain, ternary, comma, casts, `sizeof`,
`_Alignof`, `_Generic`, compound assignment, pre/post increment, and GNU
statement expressions (`({ ... })`).

**GNU/C11 extras:** `__attribute__` (`packed`, `aligned`, `weak`, `noreturn`,
`section`, `used`, …), `__builtin_*`, `typeof`, `__asm__` basic and extended,
`_Static_assert`.

Errors carry file, line and column so `../driver/util.c` can print a caret; the
first error is fatal.
