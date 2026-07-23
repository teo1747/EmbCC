# tools

Host tools that are not the compiler.

- **gen-predef.sh** — regenerates src/cpp/predef.c from the reference
  gcc (ARCHITECTURE §5). The only way that table may change.
- **embread/** — dumps and VERIFIES an EMBX image: the §6 load sequence
  and every §8 parse-time guard, run on the host so a producer bug is
  found here rather than as a one-word refusal from the kernel. It is
  the verifier the integrated linker will be tested with.
