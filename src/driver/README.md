# src/driver

argv, flags, orchestration — ../../docs/ARCHITECTURE.md §2.

M1 state: `-c FILE.c [-o FILE.o]` runs lex→parse→sema→IR→codegen→ELF
in-process; `--version`, `--dump-predef`, `--emit-empty-object` remain.
No linking (M3): invoking without -c is a loud error.
