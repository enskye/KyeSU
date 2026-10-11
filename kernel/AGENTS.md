# Kernel agent notes

Applies to everything under `kernel/`. Read the root `AGENTS.md` first; this
file adds the rules for this fork's kernel tree, which follows backslashxx's
downstream rework, not upstream tiann/KernelSU.

## Do not touch annotated code

The upstream author has marked the parts an agent must leave alone. Treat any
line using one of these, and the code it guards, as hands-off:

- `static_assert()` / `_Static_assert()`
- `assume()`
- `assume_dereferenceable()`
- `assume_restricted()`

These are deliberate micro-optimizations and compile-time contracts, mostly on
the syscall hot path (`feature/sucompat.c`, `hook/branch_link_hook_arm64.c`).
The `assume*()` macros (`kernel_includes.h`) tell the compiler facts it cannot
prove, such as a pointer being non-null or dereferenceable, so it can drop
checks. Do not remove, reorder or reword them, do not "fix" a check that looks
redundant next to one (the redundancy is the point), and do not add your own.
If a change seems to need one of them altered, stop and ask.

## Before changing kernel code

- **Unity build.** `ksu.o` is the only object (`Makefile`): `ksu.c` includes
  every header and source. Adding a file means wiring it into `ksu.c`.
- **One tree, many kernels.** The same source builds for Linux 3.x to 7.x,
  gated with `LINUX_VERSION_CODE` and `CONFIG_*`. A block under
  `#if LINUX_VERSION_CODE < ...` is usually not compiled for the 6.12 target,
  so check what your kernel actually builds before editing.
- **Downstream files are backslashxx's.** Files whose header says "downstream
  extension" (all of `downstream/` and `hook/`, plus some in `feature/`,
  `manager/` and `kernel_compat.h` / `kernel_includes.h`) are rebased from
  upstream almost daily. Keep edits minimal and local; no refactors,
  reformatting or renames.
- **Measure the hot path, don't guess.** Compare hook-function instruction
  counts (`llvm-nm --print-size ksu.ko`, size / 4) for `ksu_do_execveat_common`,
  `ksu_do_faccessat`, `ksu_vfs_fstatat` and `ksu_vfs_statx` before and after.
  On-device timing is too noisy to decide anything.
- **Stale paths.** The root `AGENTS.md` predates the directory split: e.g.
  `kernel/sucompat.c` is now `feature/sucompat.c`, `kernel/supercalls.c` is
  `supercall/`, `kernel/allowlist.c` is `policy/allowlist.c`.
