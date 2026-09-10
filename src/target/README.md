# src/target

Which machine EmbCC emits for — see ../../docs/DECISIONS.md D-011.

One process compiles for one target, chosen by `--target=` and fixed before
the front-end runs. Nothing here reads the HOST architecture for any reason:
the compiler that runs on a Mac and the one that will run on EmbLinkOS itself
must make identical objects.

## Why relocation *kinds* exist

`target_reloc_type()` maps a machine-neutral `enum reloc_kind` to an ELF
relocation type. The indirection is not ceremony — it is the one place where
the two machines disagree about *how many* relocations an act costs:

    &symbol   x86-64    lea rax, [rip+rel32]      one R_X86_64_PC32
              aarch64   adrp x9, sym              R_AARCH64_ADR_PREL_PG_HI21
                        add  x9, x9, #:lo12:sym   R_AARCH64_ADD_ABS_LO12_NC

So codegen records a KIND at each patch site (`strsite`/`gsite`/`fsite` in
../codegen/codegen.h carry it), the aarch64 backend pushes two sites where
the x86 backend pushes one, and the driver turns (kind, target) into a type
and an addend. The addend differs too: x86-64's PC-relative fields are
measured from the END of the instruction and bias by -4, aarch64's from the
instruction itself and bias by 0.

Adding a third machine means adding a column here, not a branch in the driver.
