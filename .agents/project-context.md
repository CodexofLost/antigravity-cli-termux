# Project Context: antigravity-cli-termux

## Overview
- **Repository:** `CodexofLost/antigravity-cli-termux` (Standalone Root Repository)
- **Upstream Engine:** Google Antigravity CLI (official manifest pipeline)
- **Target Environment:** Native Android Termux (`linux-aarch64`)
- **Core Goal:** Provide a relocatable, wrapper-free, glibc-compatible standalone port of the Google Antigravity CLI for Termux on Android devices.

## Upstream Synchronization Architecture
- **Direct Google Manifest Tracking:**
  - Google distributes new binary releases via an authenticated Cloud Run JSON manifest (`linux_arm64.json`).
  - `.github/scripts/check-version.sh` detects new upstream versions directly from Google's manifest and triggers automated build and release publishing on GitHub Actions.
- **Autonomous Inlined Build Pipeline:**
  - `.github/workflows/auto-sync-release.yml` builds directly from `CodexofLost/dev` with fully inlined containerized Termux testing (`termux-run.yml`).
  - 100% self-contained: zero external repository dependencies.

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
  - Automatically installs and maintains `$PREFIX/bin/agentapi`, self-heals corrupted `~/.gemini/antigravity-cli/bin/agentapi` shims, atomically wraps standalone auxiliary GRTE binaries (`webm_encoder` in `~/.gemini/antigravity-cli/bin/webm_encoder`) via glibc dynamic loader wrappers, and relies on native Termux `ripgrep` (`$PREFIX/bin/rg`) via `$PATH` for NEON-accelerated Bionic code search.
  - Automatically builds, installs, and injects `libtermux-stat-fix.so` into `termux-shell` and `termux-shell-env`, intercepting `fstatat`/`stat`/`lstat`/`statx` for `/storage/emulated` (including trailing slash paths) with strict POSIX compliance to completely eliminate Git 2.35+ `fatal: detected dubious ownership` failures across Android shared storage (`/storage/emulated/0`).
  - Sets `GIT_DISCOVERY_ACROSS_FILESYSTEM=1` to allow git discovery across Android shared storage (`/storage/emulated/0`) FUSE mount boundaries.
  - Sets `BROWSER=termux-open-url` (if unset) for native Android browser launch in OAuth authentication.
  - Guarantees UTF-8 locale (`LANG=en_US.UTF-8` fallback).
  - Automatically bootstraps Termux storage symlink hierarchy (`$HOME/storage/shared -> /storage/emulated/0`) on startup if accessible, or emits a friendly one-line permission notice if ungranted.
  - Dynamically synchronizes active Android system DNS properties (`net.dns1`, `net.dns2`) into `$PREFIX/etc/resolv.conf` and ensures `$PREFIX/glibc/etc/resolv.conf` symlinking, falling back to public nameservers (`1.1.1.1`, `8.8.8.8`) when properties are absent or on captive portals.
  - Verifies presence of `$PREFIX/etc/hosts`, automatically writing `localhost` and `ip6-localhost` mappings if missing.
  - Validates native Termux environment across interactive GUI, subshell, `tmux`, and OpenSSH (`sshd`) sessions without crashing.
- **Process Group, Subreaper & Zombie Supervision:**
  - Designates bootstrapper as subreaper via `prctl(PR_SET_CHILD_SUBREAPER, 1)` to adopt and reap orphaned grandchildren (such as JVM `maven-tools-mcp.jar` or background Node.js processes).
  - Establishes a dedicated process group (`setpgid(0, 0)`) and safely preserves interactive foreground terminal ownership (`tcsetpgrp`).
  - Sets `prctl(PR_SET_PDEATHSIG, SIGTERM)` on child engine to ensure clean teardown if parent terminates.
  - Installs signal handlers for `SIGINT`, `SIGTERM`, `SIGHUP`, `SIGQUIT` broadcasting to `kill(-getpgrp(), sig)`.
  - Non-blocking `waitpid(-1, &status, WNOHANG)` reaper on `SIGCHLD` prevents defunct/zombie process accumulation.
  - Transparently relays `SIGWINCH` to child engine on Android virtual keyboard toggle and screen orientation changes.
  - Propagates exact child exit status or signal code (`128 + sig`).
- **Subshell Shebang Execution Bridge & Non-Interactive Environment:**
  - Automatically manages `$PREFIX/libexec/agy/termux-shell` (executable `0755`) and `$PREFIX/libexec/agy/termux-shell-env`.
  - Sets `SHELL=$PREFIX/libexec/agy/termux-shell`, `BASH_ENV=$PREFIX/libexec/agy/termux-shell-env`, `ENV=$PREFIX/libexec/agy/termux-shell-env`, and `AGY_REAL_SHELL` to the user's real shell (bash/zsh).
  - Ensures all child shells (interactive and non-interactive `bash -c` / `sh -c`), subagents, and MCP background processes inherit Bionic's `libtermux-exec.so` and `libtermux-stat-fix.so`, `ANTIGRAVITY_AGENTAPI_EXE`, `TMPDIR`, and `XDG_RUNTIME_DIR`, preventing `bad interpreter` errors on shebangs, non-terminating `agentapi` function calls, and ensuring seamless Git execution across shared storage.
- **Hardware Capability Detection:**
  - Checks ARMv8.1-A LSE atomics (`HWCAP_ATOMICS`) via `getauxval(AT_HWCAP)`.
  - Falls back to running engine under `$PREFIX/bin/qemu-aarch64` if LSE is unsupported.
- **Resilient Transactional Self-Update & Semantic Versioning:**
  - Configurable repository targeting: uses `CodexofLost/antigravity-cli-termux` by default, with dynamic runtime override via `AGY_UPDATE_REPO` (validated against shell metacharacters to prevent injection).
  - Resilient network fetching: bounded curl downloads with `--connect-timeout 15 --max-time 300 --retry 3 --retry-delay 2`.
  - Pre-extraction archive integrity validation via `tar -tzf` and optional SHA256 checksum verification (`.sha256`).
  - Guaranteed ext4 staging (`$install_dir/../tmp`) preventing Android shared storage `noexec` execution check failures.
  - In-place atomic zero-copy `mv` replacements saving 400MB of flash writes per update.
  - Startup update check automatically re-execs `execv(exec_path, argv)` so newly installed bootstrapper features activate immediately.
  - Intercepts `agy update` (and startup checks) to stage and replace `agy`, `agy.va39`, and `libtermux-stat-fix.so` with rollback safety.

### 3. Self-Healing Installer (`install.sh`)
- Enforces native Termux validation (rejects PRoot).
- `ensure_dependencies()` automatically checks and installs required Termux packages: `glibc`, `glibc-repo`, `resolv-conf`, `ca-certificates`, `termux-exec-glibc`, `termux-exec`, `termux-tools`, `qemu-user-aarch64` via `pkg`.
- Auto-generates fallback `resolv.conf` and `hosts` if not present.
- Ensures `$PREFIX/tmp` exists with secure `0700` permissions.
- Installs twin binaries (`agy`, `agy.va39`) and `agentapi` CLI bridge to `$PREFIX/bin/`.

### 4. CI/CD & Automation (`.github/workflows/`)
- `auto-sync-release.yml`: 6-hour cron check directly against Google Cloud Run release manifest (`linux_arm64.json`).
- Autonomous pipeline builds directly from `CodexofLost/dev` without auto-merging upstream port.
- Cross-compilation & automated building.
- Containerized Termux smoke testing (`termux-run.yml`).
- Attested GitHub Release creation via Sigstore.
