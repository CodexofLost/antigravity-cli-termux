# Project Context: antigravity-cli-termux

## 1. Quick Specs & Project Cockpit

| Dimension | Specification |
| :--- | :--- |
| **Identifier** | `CodexofLost/antigravity-cli-termux` (`bin/agy`, `bin/agy.va39`, `bin/libtermux-stat-fix.so`) |
| **Runtime / Platform** | C (gnu11), Bash 5+, Python 3 / Android Termux (`linux-aarch64`, ARMv8.1-A LSE, fallback QEMU) |
| **Core Stack** | Termux glibc runtime (`ld-linux-aarch64.so.1`), Bionic libc interposers, Google Antigravity GRTE engine |
| **Security & Auth** | Google OAuth PKCE (`BROWSER=termux-open-url`), Sigstore OIDC provenance attestations, TLS CA bundle (`$PREFIX/etc/tls/cert.pem`) |
| **Primary Commands** | Build: `./build.sh` \| CI: `gh workflow run auto-sync-release.yml` \| Smoke: `./bin/agy --help` \| Installer: `bash install.sh` |

## 2. Component Ownership & Architecture Map

### Active Source Inventory
* `build.sh` → `detect_compiler()`, `check_prereqs()`, inline Python VA39 byte-patcher; Orchestrates binary downloads, opcode rewriting, native bootstrapper compilation, and preload library build.
* `install.sh` → `ensure_dependencies()`, `download_with_progress()`, `spinner()`; Self-healing Termux installer provisioning APT preferences, fetching twin-binaries, and validating environment.
* `lib/agy_helper.c` → `main()`, `supervise_engine_process()`, `setup_termux_environment()`, `ensure_termux_shell_bridge()`, `perform_transactional_update()`; Relocatable C bootstrapper resolving self-path, sanitizing environments, supervising engine lifecycle as subreaper, and executing in-place updates.
* `lib/termux_stat_fix.c` → `fstatat()`, `fstatat64()`, `stat()`, `stat64()`, `lstat()`, `lstat64()`, `statx()`, `access()`, `faccessat()`, `opendir()`; LD_PRELOAD interposition shim intercepting `/storage/emulated` calls to synthesize directory entries and eliminate Git dubious ownership / EACCES errors.
* `.github/scripts/check-version.sh` → `manifest_url`, `manifest_fields`, `build_needed`; Inspects Google Cloud Run manifest (`linux_arm64.json`) to detect new upstream releases.
* `.github/scripts/generate-release-notes.sh` → `VERSION`, `FORK_CHANGELOG`, `UPSTREAM_NOTES`; Generates release changelog combining fork commits, upstream `CHANGELOG.md` via `mq`, and template substitution.
* `.github/workflows/auto-sync-release.yml` → `check-and-build`, `release-installer-smoke`, `release-artifact-smoke`, `publish-release`; Autonomous 6-hour cron workflow building, testing in containerized Termux, signing via Sigstore, and publishing releases.
* `.github/workflows/termux-run.yml` → `workflow_call`, `termux-image: termux/termux-docker-pacman`; Reusable GitHub Actions workflow executing commands inside containerized Termux ARM environment.
* `.github/release-template.md` → Release notes template with placeholder tokens for version, changelog, and attestation.
* `logo.ans` → ANSI terminal logo asset displayed during installer execution.
* `README.md` → Project overview, quickstart installation commands, and architectural documentation.

### Critical Execution Traces
* **CLI Invocation:** `User runs agy → lib/agy_helper.c:main() cleans LD_PRELOAD/LD_LIBRARY_PATH → setup_termux_environment() sets TLS/DNS/TMPDIR → execv(ld-linux-aarch64.so.1, --library-path, lib_path, agy.va39, ...) → supervise_engine_process() sets subreaper/PDEATHSIG and reaps child processes`
* **Binary Patching:** `build.sh queries Google manifest → downloads upstream ELF → inline Python scans 4/8/16-byte opcode patterns → rewrites ubfx/lsl/mmap/faccessat2 instructions for 39-bit VA → asserts replacement counts > 0 → writes bin/agy.va39`
* **In-Place Self-Update:** `agy update → check_and_perform_update() fetches latest GitHub tag → compares semver → perform_transactional_update() downloads archive & sha256 to staging ext4 → validates tar integrity → atomic twin-binary rename with rollback trap → execv(exec_path, argv)`
* **Storage Interposition:** `Tool accesses /storage/emulated → libtermux-stat-fix.so intercepts fstatat/stat/opendir → returns synthetic 0755 root directory entry → bypasses Android FUSE EACCES / dubious ownership checks`

## 3. Domain & Technical Specifications

* **Endpoints & Services:**
  - Upstream Release Manifest: `https://antigravity-cli-auto-updater-974169037036.us-central1.run.app/manifests/linux_arm64.json` (HTTPS/JSON; fields `.version`, `.url`).
  - Upstream Changelog: `https://raw.githubusercontent.com/google-antigravity/antigravity-cli/refs/heads/main/CHANGELOG.md`.
  - GitHub Releases API: `https://github.com/<AGY_GITHUB_REPO>/releases/latest` (HTTP 302 location header extraction).
  - Release Asset Download: `https://github.com/<AGY_GITHUB_REPO>/releases/download/<tag>/antigravity-termux-standalone.tar.gz` and `.sha256`.
* **Data Schemas & Persistence:**
  - APT Preferences: `$PREFIX/etc/apt/preferences.d/no-shadow-glibc` (Pins 12 TUR shadow packages to priority `-1` to prevent 400MB+ unwanted package bloat).
  - DNS Configuration: `$PREFIX/etc/resolv.conf` synchronized from Android system properties (`net.dns1`, `net.dns2`) with public fallback (`1.1.1.1`, `8.8.8.8`); symlinked to `$PREFIX/glibc/etc/resolv.conf`.
  - Synthetic Emulated Storage: `$PREFIX/tmp/.synthetic_emulated` symlinked to `/storage/emulated/0`.
  - Subshell Execution Bridge: `$PREFIX/libexec/agy/termux-shell` (0755), `$PREFIX/libexec/agy/termux-shell-env` (0644).
  - Agent API CLI Shim: `$PREFIX/bin/agentapi` and `~/.gemini/antigravity-cli/bin/agentapi` (0755; self-healed to dispatch through `$PREFIX/bin/agy agentapi`).
* **Security & Auth:**
  - CA Certificates: `$PREFIX/etc/tls/cert.pem` mapped to `SSL_CERT_FILE` and `NODE_EXTRA_CA_CERTS`.
  - Cryptographic Provenance: Sigstore Keyless OIDC build attestation verified via `gh attestation verify antigravity-termux-standalone.tar.gz -R <repo>`.
  - Temp Directory Hardening: `$PREFIX/tmp` created with permission `0700` preventing Android `/tmp` (0771) write permission denial.

## 4. Architectural Invariants & Guardrails

* **State Ownership:**
  - Single Standalone Authority: All execution relies entirely on local Termux filesystem state; zero external repository submodules or wrappers.
  - Process Subreaper Ownership: The `agy` bootstrapper sets `PR_SET_CHILD_SUBREAPER` and dedicated process group (`setpgid(0, 0)`), adopting and reaping all orphaned grandchild processes (Node.js/JVM MCP servers) via non-blocking `waitpid` on `SIGCHLD`.
* **Boundaries & Subsystem Isolation:**
  - Bionic vs. Glibc Boundary: Android Bionic preloads (`LD_PRELOAD`, `LD_LIBRARY_PATH`) must remain unset when invoking the glibc dynamic linker (`ld-linux-aarch64.so.1`) to avoid `invalid ELF header` panics.
  - Child Shell Inheritance: Subshells spawned by the engine execute through `$PREFIX/libexec/agy/termux-shell` with `libtermux-exec.so` and `libtermux-stat-fix.so` re-injected, guaranteeing POSIX shebang resolution and shared storage compatibility without leaking glibc preloads to Bionic binaries.
  - Memory VA39 Ceiling: ARM64 user space is constrained to 39-bit virtual addressing; critical opcode substitutions (`ubfx`, `lsl`, `mmap`, `faccessat2`) must have match count > 0 or build aborts.
* **Budgets & Guardrails:**
  - Flash Endurance: In-place update uses staging rename (`mv -f`), saving ~400MB flash rewrites per upgrade cycle.
  - Network Timeouts: GitHub release checks bounded to 2s connect / 5s max time; downloads bounded to 15s connect / 300s max time with 3 retries.
  - Rollback Safety: Staged update trapped on `EXIT`/`INT`/`TERM`—if replacement fails before commit, original binaries are restored immediately; exits with code `125` if rollback fails.

## 5. End-to-End Data & State Flow Pipelines

* **Ingress & Patching Pipeline:**
  `Google Manifest → curl download → tar extraction → Python byte-patcher (match ubfx/lsl/mmap) → bin/agy.va39 → clang compile lib/agy_helper.c & lib/termux_stat_fix.c → bin/agy & bin/libtermux-stat-fix.so → smoke test (--help) → release archive`
* **Runtime Execution Pipeline:**
  `User invocation (agy) → check LSE (AT_HWCAP) → sanitize preloads (unset LD_PRELOAD) → setup TLS/DNS/TMPDIR → bridge termux-shell → spawn glibc dynamic loader → execute agy.va39 → supervise process group & reap defunct grandchildren`
* **Update & Self-Healing Pipeline:**
  `Startup/agy update → query GitHub latest release → parse semantic version → compare against AGY_TERMUX_VERSION → download archive & sha256 to staging ext4 → tar -tzf verify → stage to .agy.new.$$ → backup to .agy.old.$$ → atomic mv commit → self-heal ~/.gemini agentapi shims & auxiliary GRTE binaries → re-execv`

## 6. UI/UX Design System & Tokens

* `N/A`: Headless terminal CLI binary and TUI application. Terminal color sequences follow ANSI standard palette (bold, dim, green, red, cyan) for installer spinners, progress bars, and update notices.

## 7. Build, Verification & Diagnostic Recipes

* **Build (Local compile):**
  ```bash
  ./build.sh
  ```
* **CI/CD (Cloud workflow dispatch):**
  ```bash
  gh workflow run auto-sync-release.yml -f dry_run=false
  ```
* **Test (Twin-binary verification & smoke test):**
  ```bash
  ./bin/agy --help && ./bin/agy --version
  ```
* **Diagnostics (LSE atomics, glibc loader, and resolver status):**
  ```bash
  grep -q "atomics" /proc/cpuinfo && echo "LSE supported" || echo "LSE missing"
  test -x "${PREFIX:-/data/data/com.termux/files/usr}/glibc/lib/ld-linux-aarch64.so.1" && echo "Glibc loader OK"
  cat "${PREFIX:-/data/data/com.termux/files/usr}/etc/resolv.conf"
  ```
