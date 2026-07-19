# tests

- **exec/**  — programs COMPILED AND RUN, asserting output/exit code.
  These are the ones that count (CONTRIBUTING: "a change is not done because
  it compiles"). M1's `exit 42` lives here.
- **compile/** — programs that must compile, or must FAIL with a specific
  diagnostic. Failure cases matter as much as successes: THE RULE says an
  unsupported feature must fail loudly, never be silently miscompiled.
- **golden/** — output compared against gcc/TCC for agreed cases. Host-side,
  fast, and the first place a codegen regression shows up.
