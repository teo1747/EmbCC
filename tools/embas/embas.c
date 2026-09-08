/* embas — the standalone assembler CLI (A1). See src/as/as.h.
 * usage: embas [-f elf64|bin] [-o OUT] INPUT.asm  */
#include <stdio.h>
#include <string.h>

#include "../../src/as/as.h"

int main(int argc, char **argv)
{
    const char *out = "a.out", *in = NULL;
    enum as_format fmt = AS_ELF64;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o")) {
            if (++i == argc) { fprintf(stderr, "embas: -o needs a file\n"); return 2; }
            out = argv[i];
        } else if (!strcmp(argv[i], "-f")) {
            if (++i == argc) { fprintf(stderr, "embas: -f needs a format\n"); return 2; }
            if (!strcmp(argv[i], "elf64")) fmt = AS_ELF64;
            else if (!strcmp(argv[i], "bin")) fmt = AS_BIN;
            else { fprintf(stderr, "embas: unknown format '%s'\n", argv[i]); return 2; }
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "embas: unknown option '%s'\n", argv[i]); return 2;
        } else {
            in = argv[i];
        }
    }
    if (!in) { fprintf(stderr, "usage: embas [-f elf64|bin] [-o OUT] INPUT.asm\n"); return 2; }
    return as_assemble(in, out, fmt);
}
