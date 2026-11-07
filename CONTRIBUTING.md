# Contributing to Ktuyome

Thanks for your interest. Ktuyome is a small, freestanding 32-bit OS, so changes should stay lean and buildable with the existing toolchain.

## Prerequisites
- Ubuntu/Debian with: `build-essential gcc-multilib grub-pc-bin xorriso mtools qemu-system-x86`
- `make` produces `ktuyome.iso`

## Build
```sh
make clean && make
make run   # boots in QEMU with a 64 MB disk.img
```

## Guidelines
- Keep the freestanding constraints: no libc, no SSE; use `ktuyome.h` types and `kmalloc`.
- Vendor code lives under `src/vendor/`; wrap it with compat shims instead of editing upstream files when possible.
- Add new apps in `src/<name>.c`, declare them in `ktuyome.h`, register in `apps.c`, and add an icon entry.
- Update `README.md` for user-visible changes.
- Run `make clean && make` before submitting; CI does the same.

## Pull requests
- One logical change per PR.
- Include a short description and test notes (QEMU boot, app exercised).
- Do not commit `build/`, `*.iso`, or `disk.img`.