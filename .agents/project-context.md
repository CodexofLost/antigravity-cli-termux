# Project Context: antigravity-cli-termux

## Overview
- **Repository:** `CodexofLost/antigravity-cli-termux` (Forked from `wallentx/antigravity-cli-termux`)
- **Upstream Port:** `wallentx/antigravity-cli-termux` (`dev` branch)
- **Root Upstream:** Google Antigravity CLI (`google-antigravity/antigravity-cli` / manifest pipeline)
- **Target Environment:** Native Android Termux (`linux-aarch64`)
- **Core Goal:** Provide a relocatable, wrapper-free, glibc-compatible standalone port of the Google Antigravity CLI for Termux on Android devices.

## Upstream Synchronization Architecture
- **2-Tier Upstream Tracking:**
  - `upstream` (`wallentx/antigravity-cli-termux`): Tracks ongoing Termux port patches, script fixes, and memory layout adjustments.
  - `google` (`google-antigravity/antigravity-cli`): Official root repo.
- **Unified Sync Stream:**
  - Because `wallentx` merges Google's `main` into `wallentx/dev` every 6 hours, pulling from `wallentx/dev` automatically brings in both Google's official doc/repo updates and Wallentx's port features in one stream.
  - `.github/workflows/auto-sync-release.yml` automatically syncs `origin/dev` against `wallentx/antigravity-cli-termux:dev`.
- **Binary Releases:**
  - Google distributes new binary releases via an authenticated Cloud Run JSON manifest (`linux_arm64.json`).
  - `.github/scripts/check-version.sh` detects new upstream versions from this manifest and triggers automated build and release publishing on GitHub Actions.

## Architecture & Components

### 1. Binary Patching Engine (`build.sh`)
- **TCMalloc VA39 Patch:**
  - Android arm64 user space is restricted to 39-bit Virtual Address space (VA39), whereas upstream Google TCMalloc assumes standard Linux 48-bit VA space.
  - An inline Python script patches arm64 opcodes directly in the official `antigravity` ELF binary:
    - Bitmask & `ubfx` instructions (`42/44` -> `35/37`, `22/21` -> `29/28`).
    - Page-alignment constants and `mmap` masks.
    - Low-level syscall overrides (e.g. `faccessat2` -> `faccessat`).
  - Output binary: `bin/agy.va39`.
- Passes `-DAGY_GITHUB_REPO="CodexofLost/antigravity-cli-termux"` to clang when compiling the native bootstrapper.

### 2. Relocatable C Bootstrapper (`lib/agy_helper.c`)
- Compiles via `$PREFIX/bin/clang` to `bin/agy`.
- **Runtime Environment Isolation & Preload Bridging:**
  - Unsets conflicting Android Bionic preloads (`LD_PRELOAD`, `LD_LIBRARY_PATH`) that cause `invalid ELF header` panics in glibc.
  - Auto-detects glibc-native `libtermux-exec.so` (`$PREFIX/glibc/lib/libtermux-exec.so`) and passes `--preload` to `ld-linux`. This transparently intercepts `execve()` to redirect `/bin/sh`, `/bin/bash`, and `/usr/bin/env` without breaking, while cleanly stripping preloads when invoking Bionic binaries.
  - Sets `--argv0 agy` on the glibc dynamic linker so process name and CLI usage display cleanly as `agy`.
  - Sets `SSL_CERT_FILE=$PREFIX/etc/tls/cert.pem` and `GODEBUG=netdns=cgo`.
  - Guarantees UTF-8 locale (`LANG=en_US.UTF-8` fallback).
  - Verifies presence of `$PREFIX/etc/resolv.conf`, automatically writing a fallback configuration with public nameservers (`1.1.1.1`, `8.8.8.8`) if missing.
- **Hardware Capability Detection:**
  - Checks ARMv8.1-A LSE atomics (`HWCAP_ATOMICS`) via `getauxval(AT_HWCAP)`.
  - Falls back to running engine under `$PREFIX/bin/qemu-aarch64` if LSE is unsupported.
- **Self-Update & Semantic Versioning:**
  - Configurable repository targeting: uses `CodexofLost/antigravity-cli-termux` by default, with dynamic runtime override via `AGY_UPDATE_REPO`.
  - Intercepts `agy update` (and optional startup checks) to stage and replace both `agy` and `agy.va39` with rollback safety.

### 3. Self-Healing Installer (`install.sh`)
- Enforces native Termux validation (rejects PRoot).
- `ensure_dependencies()` automatically checks and installs required Termux packages: `glibc`, `glibc-repo`, `resolv-conf`, `ca-certificates`, `termux-exec-glibc`, `qemu-user-aarch64` via `pkg`.
- Auto-generates fallback `resolv.conf` if not present.
- Installs twin binaries (`agy`, `agy.va39`) to `$PREFIX/bin/`.

### 4. CI/CD & Automation (`.github/workflows/`)
- `auto-sync-release.yml`: 6-hour cron check against Google Cloud Run release manifest and `wallentx/dev` upstream branch.
- Syncs `dev` branch with `wallentx/antigravity-cli-termux:dev`.
- Cross-compilation & automated building.
- Containerized Termux smoke testing (`termux-run.yml`).
- Attested GitHub Release creation via Sigstore.
