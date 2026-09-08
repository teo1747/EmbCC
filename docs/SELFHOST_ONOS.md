# The self-hosting fixed point, on the OS

EmbCC's M3 acceptance stage 3 — `embcc-stage2` byte-identical to
`embcc-stage1` — is closed **on EmbLinkOS itself**, because stage1 links
against newlib for the OS syscall ABI and so runs on the OS, not the host
(EmbCC targets the OS's ABI, not glibc's headers). The OS is the final judge
(DECISIONS D-005), exactly as M1 and M2 were.

It has held through every change since: the source set has grown 12 → 16
(the optimizer, the DWARF emitter and the assembler each joined), and `-O0`
output stays byte-identical by construction so optimizer work never disturbs
it. EmbLD relinks on the OS too, so the whole bootstrap — compile *and* link —
runs on the metal with no cross toolchain.

## Why it can be byte-identical across host and OS

An EmbCC object depends only on the **content** of the source and the
headers it pulls in — not on the build path — because the `STT_FILE`
symbol carries the source *basename*, not the path as given. So
`src/sema/type.c` on the host and `/data/src/embcc/sema/type.c` on the OS
produce the same object, provided the headers resolve to identical bytes
in the same order. Codegen is otherwise deterministic (proven by
tests/golden/self-host.sh compiling each unit twice and diffing).

The include order must match the host self-host build exactly: EmbCC's own
freestanding headers first (so `<stdarg.h>` is EmbCC's `char*` va_list, not
newlib's), then newlib's ABI headers.

## The procedure

Host side (in the EmbCC tree):

1. Build `embcc` and generate the reference objects + relink stage1:
   ```
   make
   ./tools/gen-selfhost-ref.sh      # writes ref/*.o and myos/build/embcc.elf
   ```
   `ref/*.o` are the gcc-built embcc's output for the sixteen sources,
   compiled with `-I include -I $NEWLIB_INC` (EmbCC headers first). stage1
   (`embcc.elf`) is EmbLD-linked from those same objects.

2. Pre-flight (optional but cheap): recompile the sources from a mock OS
   layout with the OS include order and confirm byte-identity to `ref/`
   before booting anything.

OS side (in the myos tree):

3. Build the image with the EmbCC tree staged:
   ```
   rm -f embkfs.img embkfs_tree.img
   EMBK_EMBCC_ROOT=/home/motsou/EmbCC make embkfs.img
   ```
   mkfs packs the source tree to `/data/src/embcc/` (subdirs preserved so
   the relative quote-includes resolve), EmbCC's headers to
   `/data/apps/embcc/include/`, and the reference objects to
   `/data/src/embcc/ref/`.

4. Boot and run the kernel oracle: `test embcc self`. It recompiles each
   unit with
   `embcc -c <src> -I /data/apps/embcc/include -I /system/abi/include -o …`
   and compares the object to its reference byte for byte. `16/16 objects
   byte-identical` ⇒ the fixed point holds.

`test embcc` (compile the M1 program on the OS, tcc-link it, run → exit 42)
is the companion oracle: it proves stage1 is a *working* compiler on the
OS, where `test embcc self` proves it reproduces itself.
