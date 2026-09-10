# tests

- **exec/**  — programs COMPILED AND RUN, asserting output/exit code.
  These are the ones that count (CONTRIBUTING: "a change is not done because
  it compiles"). M1's `exit 42` lives here.

  How "RUN" happens depends on the target, and neither route is the host:
  `x86_64-elf` links with the host `cc` and executes directly, which only
  works where the host IS the target (a Linux x86-64 box); `aarch64-elf`
  links a bare-metal image and runs it under `qemu-system-aarch64 -M virt`,
  the machine EmbLinkOS's own ARM kernel targets. See `harness/README.md`.

      tests/run.sh                        # x86_64-elf (default)
      tests/run.sh --target=aarch64-elf   # or: make test-arm64
- **compile/** — programs that must compile, or must FAIL with a specific
  diagnostic. Failure cases matter as much as successes: THE RULE says an
  unsupported feature must fail loudly, never be silently miscompiled.
- **golden/** — output compared against gcc/TCC for agreed cases. Host-side,
  fast, and the first place a codegen regression shows up.
