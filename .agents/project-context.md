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
  - **Patch Safety Assertions:**
    - Asserts all critical opcode replacement counts (`ubfx`, `lsl`, `mask`, `mmap`) are strictly greater than zero. Fails the build immediately if upstream binary layout changes to prevent packaging segfaulting binaries.
  - Output binary: `bin/agy.va39`.
- Passes `-DAGY_GITHUB_REPO="CodexofLost/antigravity-cli-termux"` to clang when compiling the native bootstrapper.

### 2. Relocatable C Bootstrapper (`lib/agy_helper.c`)
- Compiles via `$PREFIX/bin/clang` to `bin/agy`.
- **Runtime Environment Isolation & Preload Bridging:**
  - Unsets conflicting Android Bionic preloads (`LD_PRELOAD`, `LD_LIBRARY_PATH`) that cause `invalid ELF header` panics in glibc.
  - Auto-detects glibc-native `libtermux-exec.so` (`$PREFIX/glibc/lib/libtermux-exec.so`) and passes `--preload` to `ld-linux`. This transparently intercepts `execve()` to redirect `/bin/sh`, `/bin/bash`, and `/usr/bin/env` without breaking, while cleanly stripping preloads when invoking Bionic binaries.
  - Sets `--argv0 agy` on the glibc dynamic linker so process name and CLI usage display cleanly as `agy`.
  - Sets `SSL_CERT_FILE=$PREFIX/etc/tls/cert.pem` and `GODEBUG=netdns=cgo`.
  - Configures `NODE_EXTRA_CA_CERTS=$PREFIX/etc/tls/cert.pem` for Node.js MCP server TLS verification.
  - Ensures `TMPDIR=$PREFIX/tmp` and `XDG_RUNTIME_DIR=$PREFIX/tmp` exist with permissions `0700` (avoiding Android system `/tmp` 0771 permission denials, FUSE socket bind failures, and 108-byte sockaddr_un path overflows).
  - Explicitly injects `ANTIGRAVITY_AGENTAPI_EXE=$PREFIX/bin/agy` to prevent Go's `/proc/self/exe` resolution from defaulting to `ld-linux-aarch64.so.1`.
  - Automatically installs and maintains `$PREFIX/bin/agentapi` and self-heals corrupted `~/.gemini/antigravity-cli/bin/agentapi` shims.
  - Sets `GIT_DISCOVERY_ACROSS_FILESYSTEM=1` to allow git discovery across Android shared storage (`/storage/emulated/0`) FUSE mount boundaries.
  - Sets `BROWSER=termux-open-url` (if unset) for native Android browser launch in OAuth authentication.
  - Guarantees UTF-8 locale (`LANG=en_US.UTF-8` fallback).
  - Verifies presence of `$PREFIX/etc/resolv.conf`, automatically writing a fallback configuration with public nameservers (`1.1.1.1`, `8.8.8.8`) if missing.
  - Verifies presence of `$PREFIX/etc/hosts`, automatically writing `localhost` and `ip6-localhost` mappings if missing.
- **Subshell Shebang Execution Bridge & Non-Interactive Environment:**
  - Automatically manages `$PREFIX/libexec/agy/termux-shell` (executable `0755`) and `$PREFIX/libexec/agy/termux-shell-env`.
  - Sets `SHELL=$PREFIX/libexec/agy/termux-shell`, `BASH_ENV=$PREFIX/libexec/agy/termux-shell-env`, `ENV=$PREFIX/libexec/agy/termux-shell-env`, and `AGY_REAL_SHELL` to the user's real shell (bash/zsh).
  - Ensures all child shells (interactive and non-interactive `bash -c` / `sh -c`), subagents, and MCP background processes inherit Bionic's `libtermux-exec.so`, `ANTIGRAVITY_AGENTAPI_EXE`, `TMPDIR`, and `XDG_RUNTIME_DIR`, preventing `bad interpreter: No such file or directory` errors on scripts with `#!/bin/bash` or `#!/usr/bin/env`.
- **Hardware Capability Detection:**
  - Checks ARMv8.1-A LSE atomics (`HWCAP_ATOMICS`) via `getauxval(AT_HWCAP)`.
  - Falls back to running engine under `$PREFIX/bin/qemu-aarch64` if LSE is unsupported.
- **Resilient Transactional Self-Update & Semantic Versioning:**
  - Configurable repository targeting: uses `CodexofLost/antigravity-cli-termux` by default, with dynamic runtime override via `AGY_UPDATE_REPO`.
  - Resilient network fetching: bounded curl downloads with `--connect-timeout 15 --max-time 300 --retry 3 --retry-delay 2`.
  - Guaranteed ext4 staging (`$install_dir/../tmp`) preventing Android shared storage `noexec` execution check failures.
  - In-place atomic zero-copy `mv` replacements saving 400MB of flash writes per update.
  - Startup update check automatically re-execs `execv(exec_path, argv)` so newly installed bootstrapper features activate immediately.
  - Intercepts `agy update` (and optional startup checks) to stage and replace both `agy` and `agy.va39` with rollback safety.

### 3. Self-Healing Installer (`install.sh`)
- Enforces native Termux validation (rejects PRoot).
- `ensure_dependencies()` automatically checks and installs required Termux packages: `glibc`, `glibc-repo`, `resolv-conf`, `ca-certificates`, `termux-exec-glibc`, `termux-exec`, `termux-tools`, `qemu-user-aarch64` via `pkg`.
- Auto-generates fallback `resolv.conf` and `hosts` if not present.
- Ensures `$PREFIX/tmp` exists with secure `0700` permissions.
- Installs twin binaries (`agy`, `agy.va39`) and `agentapi` CLI bridge to `$PREFIX/bin/`.

### 4. CI/CD & Automation (`.github/workflows/`)
- `auto-sync-release.yml`: 6-hour cron check against Google Cloud Run release manifest and `wallentx/dev` upstream branch.
- Syncs `dev` branch with `wallentx/antigravity-cli-termux:dev`.
- Cross-compilation & automated building.
- Containerized Termux smoke testing (`termux-run.yml`).
- Attested GitHub Release creation via Sigstore.
