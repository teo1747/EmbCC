/* embld — the standalone EmbLD driver.
 *
 * A thin argv wrapper over src/link/. The linker is a library so the
 * compiler can call it in-process (ARCHITECTURE §1: the target has no
 * fork/exec); this binary exists for host-side development and testing,
 * the way `embread` gives the ELF writer a testable front door.
 *
 * usage: embld [-o OUT] [-e ENTRY] [-Ttext ADDR]
 *              [--embx [--cap NAME]...] INPUT.o|INPUT.a ...
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/link/link.h"
#include "../../src/embx/embx.h"

int main(int argc, char **argv)
{
    const char *out = "a.out";
    struct link_opts opts;
    const char *inputs[256];
    int ninputs = 0;

    memset(&opts, 0, sizeof opts);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            if (++i == argc) { fprintf(stderr, "embld: -o needs a file\n"); return 2; }
            out = argv[i];
        } else if (strcmp(argv[i], "-e") == 0) {
            if (++i == argc) { fprintf(stderr, "embld: -e needs a symbol\n"); return 2; }
            opts.entry = argv[i];
        } else if (strncmp(argv[i], "-Ttext", 6) == 0) {
            const char *v = argv[i][6] ? argv[i] + 6
                                       : (++i < argc ? argv[i] : NULL);
            if (!v) { fprintf(stderr, "embld: -Ttext needs an address\n"); return 2; }
            opts.base = strtoul(v, NULL, 0);
        } else if (strncmp(argv[i], "--lma-offset", 12) == 0) {
            /* L2: p_paddr = p_vaddr - OFFSET, for a higher-half kernel (OFFSET =
             * KERNEL_VIRTUAL_BASE). Accepts --lma-offset=HEX or a separate arg. */
            const char *v = argv[i][12] == '=' ? argv[i] + 13
                          : (++i < argc ? argv[i] : NULL);
            if (!v) { fprintf(stderr, "embld: --lma-offset needs a value\n"); return 2; }
            opts.lma_offset = strtoull(v, NULL, 0);
        } else if (strcmp(argv[i], "--embx") == 0) {
            opts.emit_embx = 1;            /* write a native EMBX, not ELF */
        } else if (strcmp(argv[i], "--cap") == 0) {
            if (++i == argc) { fprintf(stderr, "embld: --cap needs a name\n"); return 2; }
            int id = embx_cap_id(argv[i]);
            if (id <= 0) { fprintf(stderr, "embld: unknown capability '%s'\n", argv[i]); return 2; }
            opts.caps |= (1ULL << id);     /* declare it in the EMBX cap table */
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "embld: unknown option '%s'\n", argv[i]);
            return 2;
        } else {
            if (ninputs == 256) { fprintf(stderr, "embld: too many inputs\n"); return 2; }
            inputs[ninputs++] = argv[i];
        }
    }
    if (!ninputs) {
        fprintf(stderr, "usage: embld [-o OUT] [-e ENTRY] [-Ttext ADDR]\n"
                        "             [--embx [--cap NAME]...] INPUT.o|INPUT.a ...\n");
        return 2;
    }
    return embld_link(inputs, ninputs, out, &opts);
}
