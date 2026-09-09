#include <arpa/inet.h>
#include <asm/hwcap.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef HWCAP_ATOMICS
#define HWCAP_ATOMICS (1 << 8)
#endif

#ifndef PR_SET_CHILD_SUBREAPER
#define PR_SET_CHILD_SUBREAPER 36
#endif

#ifndef PR_SET_PDEATHSIG
#define PR_SET_PDEATHSIG 1
#endif

#ifndef AGY_TERMUX_VERSION
#define AGY_TERMUX_VERSION "1.1.28"
#endif

#ifndef AGY_GITHUB_REPO
#define AGY_GITHUB_REPO "CodexofLost/antigravity-cli-termux"
#endif

static const char *get_agy_repo(void) {
    const char *env_repo = getenv("AGY_UPDATE_REPO");
    if (env_repo != NULL && env_repo[0] != '\0') {
        return env_repo;
    }
    return AGY_GITHUB_REPO;
}

enum update_check_mode {
    UPDATE_CHECK_EXPLICIT,
    UPDATE_CHECK_STARTUP,
};

static int agy_is_valid_release_tag(const char *tag) {
    if (tag == NULL || tag[0] == '\0' || tag[0] == '-') {
        return 0;
    }

    for (const unsigned char *cursor = (const unsigned char *)tag; *cursor != '\0'; cursor++) {
        if (!isalnum(*cursor) && *cursor != '.' && *cursor != '_' && *cursor != '-') {
            return 0;
        }
    }

    return 1;
}

struct semantic_version {
    unsigned long core[3];
    const char *prerelease;
    size_t prerelease_length;
};

struct version_identifier {
    const char *start;
    size_t length;
    int numeric;
};

static int validate_version_identifier(int enforce_numeric_leading_zero, const char *start,
                                       size_t length) {
    int numeric = 1;

    if (length == 0) {
        return 0;
    }

    for (size_t index = 0; index < length; index++) {
        unsigned char character = (unsigned char)start[index];
        if (!isalnum(character) && character != '-') {
            return 0;
        }
        if (!isdigit(character)) {
            numeric = 0;
        }
    }

    return !(enforce_numeric_leading_zero && numeric && length > 1 && start[0] == '0');
}

static int validate_identifier_list(int enforce_numeric_leading_zero, const char *start,
                                    size_t length) {
    size_t identifier_start = 0;

    if (length == 0) {
        return 0;
    }

    for (;;) {
        size_t identifier_end = identifier_start;
        while (identifier_end < length && start[identifier_end] != '.') {
            identifier_end++;
        }

        if (!validate_version_identifier(enforce_numeric_leading_zero, start + identifier_start,
                                         identifier_end - identifier_start)) {
            return 0;
        }
        if (identifier_end == length) {
            return 1;
        }
        identifier_start = identifier_end + 1;
    }
}

static int parse_core_component(const char **cursor, unsigned long *value) {
    if (!isdigit((unsigned char)**cursor)) {
        return 0;
    }
    if (**cursor == '0' && isdigit((unsigned char)(*cursor)[1])) {
        return 0;
    }

    errno = 0;
    char *end = NULL;
    *value = strtoul(*cursor, &end, 10);
    if (errno == ERANGE || end == *cursor) {
        return 0;
    }

    *cursor = end;
    return 1;
}

static int parse_semantic_version(const char *text, struct semantic_version *version) {
    const char *cursor = text;

    memset(version, 0, sizeof(*version));
    if (*cursor == 'v') {
        cursor++;
    }

    for (size_t component = 0; component < 3; component++) {
        if (!parse_core_component(&cursor, &version->core[component])) {
            return 0;
        }
        if (component == 2) {
            break;
        }
        if (*cursor != '.') {
            return 0;
        }
        cursor++;
    }

    if (*cursor == '-') {
        const char *prerelease_start = ++cursor;
        while (*cursor != '\0' && *cursor != '+') {
            cursor++;
        }
        version->prerelease = prerelease_start;
        version->prerelease_length = (size_t)(cursor - prerelease_start);
        if (!validate_identifier_list(1, version->prerelease, version->prerelease_length)) {
            return 0;
        }
    }

    if (*cursor == '+') {
        const char *build_start = ++cursor;
        while (*cursor != '\0') {
            cursor++;
        }
        if (!validate_identifier_list(0, build_start, (size_t)(cursor - build_start))) {
            return 0;
        }
    }

    return *cursor == '\0';
}

static struct version_identifier next_version_identifier(const char **cursor, size_t *remaining) {
    struct version_identifier identifier = {
        .start = *cursor,
        .length = 0,
        .numeric = 1,
    };

    while (identifier.length < *remaining && (*cursor)[identifier.length] != '.') {
        if (!isdigit((unsigned char)(*cursor)[identifier.length])) {
            identifier.numeric = 0;
        }
        identifier.length++;
    }

    size_t consumed = identifier.length;
    if (consumed < *remaining) {
        consumed++;
    }
    *cursor += consumed;
    *remaining -= consumed;
    return identifier;
}

static int compare_version_identifiers(const struct version_identifier *candidate,
                                       const struct version_identifier *installed) {
    if (candidate->numeric != installed->numeric) {
        return candidate->numeric ? -1 : 1;
    }

    if (candidate->numeric && candidate->length != installed->length) {
        return candidate->length < installed->length ? -1 : 1;
    }

    size_t common_length =
        candidate->length < installed->length ? candidate->length : installed->length;
    int lexical = memcmp(candidate->start, installed->start, common_length);
    if (lexical != 0) {
        return lexical < 0 ? -1 : 1;
    }
    if (candidate->length == installed->length) {
        return 0;
    }

    return candidate->length < installed->length ? -1 : 1;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static int compare_prerelease_versions(const struct semantic_version *candidate,
                                       const struct semantic_version *installed) {
    if (candidate->prerelease_length == 0 || installed->prerelease_length == 0) {
        if (candidate->prerelease_length == installed->prerelease_length) {
            return 0;
        }
        return candidate->prerelease_length == 0 ? 1 : -1;
    }

    const char *candidate_cursor = candidate->prerelease;
    const char *installed_cursor = installed->prerelease;
    size_t candidate_remaining = candidate->prerelease_length;
    size_t installed_remaining = installed->prerelease_length;

    while (candidate_remaining > 0 && installed_remaining > 0) {
        struct version_identifier candidate_identifier =
            next_version_identifier(&candidate_cursor, &candidate_remaining);
        struct version_identifier installed_identifier =
            next_version_identifier(&installed_cursor, &installed_remaining);
        int comparison = compare_version_identifiers(&candidate_identifier, &installed_identifier);
        if (comparison != 0) {
            return comparison;
        }
    }

    if (candidate_remaining == installed_remaining) {
        return 0;
    }
    return candidate_remaining == 0 ? -1 : 1;
}

// Returns -1 when candidate is older, 0 when equal, 1 when newer, and -2 when invalid.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static int compare_release_versions(const char *candidate_text, const char *installed_text) {
    struct semantic_version candidate;
    struct semantic_version installed;

    if (!parse_semantic_version(candidate_text, &candidate) ||
        !parse_semantic_version(installed_text, &installed)) {
        return -2;
    }

    for (size_t component = 0; component < 3; component++) {
        if (candidate.core[component] != installed.core[component]) {
            return candidate.core[component] < installed.core[component] ? -1 : 1;
        }
    }

    return compare_prerelease_versions(&candidate, &installed);
}

static void print_update_usage(void) {
    printf("Usage: agy update [options]\n\n"
           "Options:\n"
           "  -y, --yes, --auto  Apply updates without prompting\n"
           "  -h, --help         Show this help message\n\n"
           "Environment:\n"
           "  AGY_AUTO_UPDATE=1   Apply updates without prompting\n"
           "  AGY_UPDATE_DEBUG=1  Show startup update-check errors on stderr\n");
}

static int env_var_enabled(const char *name) {
    const char *value = getenv(name);

    return value != NULL && (strcmp(value, "1") == 0 || strcmp(value, "true") == 0);
}

static void report_update_check_error(enum update_check_mode mode, const char *message) {
    if (mode == UPDATE_CHECK_EXPLICIT) {
        printf("[agy-termux] Error: %s\n", message);
    } else if (env_var_enabled("AGY_UPDATE_DEBUG")) {
        (void)fprintf(stderr, "[agy-termux] Automatic update check failed: %s\n", message);
    }
}

static int extract_release_tag(const char *release_url, char *tag, size_t tag_size) {
    char tag_prefix[PATH_MAX];
    int written = snprintf(tag_prefix, sizeof(tag_prefix), "https://github.com/%s/releases/tag/",
                           get_agy_repo());
    if (written < 0 || written >= (int)sizeof(tag_prefix)) {
        return 0;
    }

    const size_t prefix_length = (size_t)written;
    const char *tag_start = NULL;
    size_t tag_length = 0;

    if (strncmp(release_url, tag_prefix, prefix_length) != 0) {
        return 0;
    }

    tag_start = release_url + prefix_length;
    tag_length = strlen(tag_start);
    if (tag_length == 0 || tag_length >= tag_size || !agy_is_valid_release_tag(tag_start)) {
        return 0;
    }

    memcpy(tag, tag_start, tag_length + 1);
    return 1;
}

static int fetch_latest_release_tag(enum update_check_mode mode, char *latest_tag,
                                    size_t latest_tag_size) {
    char command[768];
    char release_url[PATH_MAX] = {0};
    int written = snprintf(command, sizeof(command),
                           "command -v curl >/dev/null 2>&1 && "
                           "curl --proto '=https' --tlsv1.2 --connect-timeout 2 --max-time 5 -fLsL "
                           "-o /dev/null -w '%%{url_effective}\\n' -H 'User-Agent: Termux-Agy' "
                           "'https://github.com/%s/releases/latest'",
                           get_agy_repo());
    if (written < 0 || written >= (int)sizeof(command)) {
        report_update_check_error(mode, "could not construct the release query");
        return 0;
    }

    // Intentionally uses the shell for a bounded curl request.
    // NOLINTNEXTLINE(bugprone-command-processor,cert-env33-c,clang-analyzer-optin.taint.GenericTaint)
    FILE *pipe = popen(command, "r");
    if (pipe == NULL) {
        report_update_check_error(mode, "could not start the release query");
        return 0;
    }

    int received_line = fgets(release_url, sizeof(release_url), pipe) != NULL;
    int extra_output = received_line ? fgetc(pipe) : EOF;
    int command_status = pclose(pipe);

    if (command_status == -1 || !WIFEXITED(command_status) || WEXITSTATUS(command_status) != 0) {
        report_update_check_error(mode, "GitHub release query did not succeed");
        return 0;
    }
    if (!received_line || extra_output != EOF || strchr(release_url, '\n') == NULL) {
        report_update_check_error(mode, "GitHub returned an unexpected release response");
        return 0;
    }

    release_url[strcspn(release_url, "\r\n")] = '\0';
    if (!extract_release_tag(release_url, latest_tag, latest_tag_size)) {
        report_update_check_error(mode, "GitHub returned an unsupported release URL");
        return 0;
    }

    return 1;
}

static int should_perform_update(int auto_update) {
    if (auto_update) {
        printf("[agy-termux] Proceeding with automatic update (non-interactive)...\n");
        return 1;
    }

    if (!isatty(STDIN_FILENO)) {
        printf("[agy-termux] Error: standard input is not a TTY and auto-update is not enabled.\n");
        printf("[agy-termux] Run `agy update -y` or set AGY_AUTO_UPDATE=1 for non-interactive "
               "updates.\n");
        return 0;
    }

    for (;;) {
        printf("[agy-termux] Would you like to update now? [Y/n]: ");
        (void)fflush(stdout);

        char response_line[64] = {0};
        if (fgets(response_line, sizeof(response_line), stdin) == NULL) {
            return 0;
        }
        if (strchr(response_line, '\n') == NULL) {
            int ch = 0;
            while ((ch = getchar()) != '\n' && ch != EOF) {
            }
        }

        if (response_line[0] == '\n' || response_line[0] == '\0') {
            return 1;
        }
        if (response_line[0] == 'y' || response_line[0] == 'Y') {
            return 1;
        }
        if (response_line[0] == 'n' || response_line[0] == 'N') {
            return 0;
        }

        printf("[agy-termux] Invalid selection. Enter y or n.\n");
    }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static int perform_transactional_update(const char *dir, const char *latest_tag) {
    char update_cmd[8192];
    int written = snprintf(
        update_cmd, sizeof(update_cmd),
        "install_dir=\"%s\"; release_tag=\"%s\"; "
        "staging_base=\"$install_dir/../tmp\"; "
        "[ -d \"$staging_base\" ] || "
        "staging_base=\"${TMPDIR:-/data/data/com.termux/files/usr/tmp}\"; "
        "tmp=$(mktemp -d \"$staging_base/agy-update.XXXXXX\") || exit 1; "
        "new_agy=\"$install_dir/.agy.new.$$\"; "
        "new_payload=\"$install_dir/.agy.va39.new.$$\"; "
        "old_agy=\"$install_dir/.agy.old.$$\"; "
        "old_payload=\"$install_dir/.agy.va39.old.$$\"; committed=0; "
        "cleanup() { status=$?; trap - EXIT HUP INT TERM; rollback_failed=0; "
        "if [ \"$committed\" -eq 0 ]; then "
        "[ ! -e \"$old_agy\" ] || mv -f \"$old_agy\" \"$install_dir/agy\" || "
        "rollback_failed=1; "
        "[ ! -e \"$old_payload\" ] || "
        "mv -f \"$old_payload\" \"$install_dir/agy.va39\" || rollback_failed=1; "
        "fi; "
        "rm -f \"$new_agy\" \"$new_payload\"; "
        "rm -rf \"$tmp\"; "
        "if [ \"$rollback_failed\" -ne 0 ]; then exit 125; fi; exit \"$status\"; }; "
        "trap cleanup EXIT; trap 'exit 129' HUP; trap 'exit 130' INT; trap 'exit 143' TERM; "
        "curl --proto '=https' --tlsv1.2 --connect-timeout 15 --max-time 300 --retry 3 "
        "--retry-delay 2 "
        "-fLsL -o \"$tmp/antigravity-termux-standalone.tar.gz\" "
        "\"https://github.com/%s/releases/download/"
        "$release_tag/antigravity-termux-standalone.tar.gz\" && "
        "sha_url=\"https://github.com/%s/releases/download/"
        "$release_tag/antigravity-termux-standalone.tar.gz.sha256\"; "
        "if curl --proto '=https' --tlsv1.2 --connect-timeout 10 --max-time 30 -fLsL "
        "-o \"$tmp/archive.sha256\" \"$sha_url\" 2>/dev/null; then "
        "expected_sha=$(awk '{print $1}' \"$tmp/archive.sha256\"); "
        "actual_sha=$(sha256sum \"$tmp/antigravity-termux-standalone.tar.gz\" | awk '{print $1}'); "
        "if [ -n \"$expected_sha\" ] && [ -n \"$actual_sha\" ] && "
        "[ \"$expected_sha\" != \"$actual_sha\" ]; then "
        "printf '[agy-termux] Error: SHA256 checksum mismatch.\\n' >&2; exit 1; fi; "
        "fi; "
        "tar -tzf \"$tmp/antigravity-termux-standalone.tar.gz\" >/dev/null && "
        "tar -xzf \"$tmp/antigravity-termux-standalone.tar.gz\" -C \"$tmp\" "
        "agy agy.va39 && "
        "test -s \"$tmp/agy\" && test -x \"$tmp/agy\" && "
        "test -s \"$tmp/agy.va39\" && test -x \"$tmp/agy.va39\" && "
        "\"$tmp/agy\" --help >/dev/null 2>&1 && "
        "install -m 0755 \"$tmp/agy\" \"$new_agy\" && "
        "install -m 0755 \"$tmp/agy.va39\" \"$new_payload\" && "
        "mv -f \"$install_dir/agy\" \"$old_agy\" && "
        "mv -f \"$install_dir/agy.va39\" \"$old_payload\" && "
        "mv -f \"$new_payload\" \"$install_dir/agy.va39\" && "
        "mv -f \"$new_agy\" \"$install_dir/agy\" && "
        "committed=1 && { rm -f \"$old_agy\" \"$old_payload\" || :; }",
        dir, latest_tag, get_agy_repo(), get_agy_repo());
    if (written < 0 || written >= (int)sizeof(update_cmd)) {
        return -1;
    }

    // Intentionally uses the shell for a staged, rollback-safe two-file replacement.
    // NOLINTNEXTLINE(bugprone-command-processor,cert-env33-c,cert-err34-c,cert-str02-c,clang-analyzer-optin.taint.GenericTaint)
    return system(update_cmd);
}

// Query this fork's latest release and update the installed twin binaries in place.
static int check_and_perform_update(enum update_check_mode mode, const char *dir, int auto_update) {
    char latest_tag[64] = {0};
    if (mode == UPDATE_CHECK_EXPLICIT) {
        printf("[agy-termux] Querying latest release from %s...\n", get_agy_repo());
    }
    if (!fetch_latest_release_tag(mode, latest_tag, sizeof(latest_tag))) {
        return 0;
    }

    const char *clean_latest = (latest_tag[0] == 'v') ? latest_tag + 1 : latest_tag;
    const char *clean_current =
        (AGY_TERMUX_VERSION[0] == 'v') ? &AGY_TERMUX_VERSION[1] : AGY_TERMUX_VERSION;
    int version_comparison = compare_release_versions(clean_latest, clean_current);

    if (mode == UPDATE_CHECK_EXPLICIT) {
        printf("[agy-termux] Current standalone version: v%s\n", clean_current);
        printf("[agy-termux] Latest available version : v%s\n", clean_latest);
    }

    if (version_comparison == -2) {
        report_update_check_error(mode, "could not compare installed and available versions");
        return 0;
    }
    if (version_comparison <= 0) {
        if (mode == UPDATE_CHECK_EXPLICIT) {
            if (version_comparison == 0) {
                printf("[agy-termux] You are already up to date with the latest standalone "
                       "release.\n");
            } else {
                printf("[agy-termux] Installed version v%s is newer than latest release v%s; "
                       "no update applied.\n",
                       clean_current, clean_latest);
            }
        }
        return 0;
    }

    printf("\n[agy-termux] A new update (v%s) is available!\n", clean_latest);
    if (!should_perform_update(auto_update)) {
        printf("[agy-termux] Update cancelled.\n");
        return 0;
    }

    printf("\n[agy-termux] Downloading and applying standalone update...\n");
    int status = perform_transactional_update(dir, latest_tag);
    if (status == 0) {
        if (mode == UPDATE_CHECK_STARTUP) {
            printf("[agy-termux] Update completed successfully. Starting the CLI...\n");
        } else {
            printf("[agy-termux] Update completed successfully! Please restart the CLI.\n");
        }
        return 1;
    }

    if (status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 125) {
        printf("[agy-termux] Error: Update failed and rollback could not be completed.\n");
    } else {
        printf("[agy-termux] Error: Update failed; installed binaries were left unchanged or "
               "restored.\n");
    }
    return 0;
}

static int is_update_help_flag(const char *arg) {
    return strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0;
}

static int is_update_auto_flag(const char *arg) {
    return strcmp(arg, "-y") == 0 || strcmp(arg, "--yes") == 0 || strcmp(arg, "--auto") == 0;
}

static int update_command_requests_help(int argc, char **argv) {
    for (int i = 2; i < argc; i++) {
        if (is_update_help_flag(argv[i])) {
            return 1;
        }
    }

    return 0;
}

static int is_update_command(int argc, char **argv) {
    return argc >= 2 && strcmp(argv[1], "update") == 0;
}

static int env_requests_auto_update(void) {
    return env_var_enabled("AGY_AUTO_UPDATE");
}

static int handle_update_command(const char *dir, int argc, char **argv) {
    int auto_update = env_requests_auto_update();

    for (int i = 2; i < argc; i++) {
        if (is_update_help_flag(argv[i])) {
            print_update_usage();
            return 0;
        }
        if (is_update_auto_flag(argv[i])) {
            auto_update = 1;
        }
    }

    (void)check_and_perform_update(UPDATE_CHECK_EXPLICIT, dir, auto_update);
    return 0;
}

static int should_check_for_update_on_startup(int argc, char *const *argv) {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        return 0;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0 ||
            strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            return 0;
        }
    }

    return 1;
}

static int perform_startup_update_check(const char *dir, int argc, char **argv) {
    if (should_check_for_update_on_startup(argc, argv)) {
        return check_and_perform_update(UPDATE_CHECK_STARTUP, dir, env_requests_auto_update());
    }
    return 0;
}

static int is_native_termux(void) {
    const char *termux_version = getenv("TERMUX_VERSION");
    const char *prefix = getenv("PREFIX");
    char bin_path[PATH_MAX];
    int written = 0;

    if (termux_version == NULL || termux_version[0] == '\0') {
        return 0;
    }
    if (prefix == NULL || prefix[0] == '\0') {
        return 0;
    }
    written = snprintf(bin_path, sizeof(bin_path), "%s/bin", prefix);
    if (written < 0 || written >= (int)sizeof(bin_path)) {
        return 0;
    }
    if (access(bin_path, F_OK) != 0) {
        return 0;
    }

    return 1;
}

static void print_non_termux_message(void) {
    (void)fprintf(stderr, "[agy-termux] This standalone port is only for native Termux.\n"
                          "[agy-termux] PRoot environments can use Google's official "
                          "Antigravity CLI binary directly.\n"
                          "[agy-termux] Install it with:\n"
                          "  curl -fsSL https://antigravity.google/cli/install.sh | bash\n");
}

static int is_valid_ip_address(const char *ip) {
    if (ip == NULL || ip[0] == '\0') {
        return 0;
    }

    struct in_addr addr4;
    if (inet_pton(AF_INET, ip, &addr4) == 1) {
        return 1;
    }

    struct in6_addr addr6;
    if (inet_pton(AF_INET6, ip, &addr6) == 1) {
        return 1;
    }

    return 0;
}

static int fetch_android_dns_property(const char *prop, char *buffer, size_t buffer_size) {
    char cmd[128];
    const char *getprop_bin =
        (access("/system/bin/getprop", X_OK) == 0) ? "/system/bin/getprop" : "getprop";
    int written = snprintf(cmd, sizeof(cmd), "%s %s", getprop_bin, prop);
    if (written < 0 || written >= (int)sizeof(cmd)) {
        return 0;
    }

    // NOLINTNEXTLINE(bugprone-command-processor,cert-env33-c,clang-analyzer-optin.taint.GenericTaint)
    FILE *pipe = popen(cmd, "r");
    if (pipe == NULL) {
        return 0;
    }

    buffer[0] = '\0';
    if (fgets(buffer, (int)buffer_size, pipe) != NULL) {
        buffer[strcspn(buffer, "\r\n")] = '\0';
    }
    int prop_status = pclose(pipe);
    if (prop_status != 0 || buffer[0] == '\0') {
        return 0;
    }

    return is_valid_ip_address(buffer);
}

static int require_resolver_config(const char *prefix) {
    char resolv_path[PATH_MAX];
    char glibc_resolv_path[PATH_MAX];
    char glibc_etc_dir[PATH_MAX];

    int written = snprintf(resolv_path, sizeof(resolv_path), "%s/etc/resolv.conf", prefix);
    if (written < 0 || written >= (int)sizeof(resolv_path)) {
        return 0;
    }
    written =
        snprintf(glibc_resolv_path, sizeof(glibc_resolv_path), "%s/glibc/etc/resolv.conf", prefix);
    if (written < 0 || written >= (int)sizeof(glibc_resolv_path)) {
        return 0;
    }
    written = snprintf(glibc_etc_dir, sizeof(glibc_etc_dir), "%s/glibc/etc", prefix);
    if (written < 0 || written >= (int)sizeof(glibc_etc_dir)) {
        return 0;
    }

    char dns1[64] = {0};
    char dns2[64] = {0};
    int has_dns1 = fetch_android_dns_property("net.dns1", dns1, sizeof(dns1));
    int has_dns2 = fetch_android_dns_property("net.dns2", dns2, sizeof(dns2));

    if (has_dns1 || has_dns2) {
        FILE *fp = fopen(resolv_path, "w");
        if (fp != NULL) {
            (void)fputs("options timeout:2 attempts:2\n", fp);
            if (has_dns1) {
                (void)fprintf(fp, "nameserver %s\n", dns1);
            }
            if (has_dns2 && strcmp(dns1, dns2) != 0) {
                (void)fprintf(fp, "nameserver %s\n", dns2);
            }
            (void)fputs("nameserver 1.1.1.1\n"
                        "nameserver 8.8.8.8\n",
                        fp);
            (void)fclose(fp);
        }
    } else if (access(resolv_path, R_OK) != 0) {
        // Auto-generate a fallback resolv.conf with reliable public DNS
        FILE *fp = fopen(resolv_path, "w");
        if (fp != NULL) {
            (void)fputs("options timeout:2 attempts:2\n"
                        "nameserver 1.1.1.1\n"
                        "nameserver 8.8.8.8\n"
                        "nameserver 8.8.4.4\n",
                        fp);
            (void)fclose(fp);
        }
    }

    // Ensure glibc resolv.conf is linked or synchronized
    if (access(glibc_resolv_path, F_OK) != 0) {
        if (access(glibc_etc_dir, F_OK) != 0) {
            (void)mkdir(glibc_etc_dir, 0755);
        }
        (void)symlink(resolv_path, glibc_resolv_path);
    }

    if (access(resolv_path, R_OK) == 0 || access(glibc_resolv_path, R_OK) == 0) {
        return 1;
    }

    (void)fprintf(stderr, "[agy-termux] Missing resolver configuration: %s\n", resolv_path);
    (void)fprintf(stderr, "[agy-termux] Install it with: pkg install resolv-conf\n");
    (void)fprintf(stderr, "[agy-termux] Without this file, login and OAuth network requests may "
                          "fail.\n");
    return 0;
}

static int ensure_hosts_config(const char *prefix) {
    char hosts_path[PATH_MAX];
    int written = snprintf(hosts_path, sizeof(hosts_path), "%s/etc/hosts", prefix);
    if (written < 0 || written >= (int)sizeof(hosts_path)) {
        return 0;
    }

    if (access(hosts_path, R_OK) == 0) {
        return 1;
    }

    FILE *fp = fopen(hosts_path, "w");
    if (fp != NULL) {
        (void)fputs("127.0.0.1 localhost\n"
                    "::1 localhost ip6-localhost\n",
                    fp);
        (void)fclose(fp);
        return access(hosts_path, R_OK) == 0;
    }

    return 0;
}

static void self_heal_agentapi_shim(const char *prefix) {
    const char *home = getenv("HOME");
    char bin_dir[PATH_MAX];
    char shim_path[PATH_MAX];
    char buffer[256];
    FILE *fp = NULL;
    int needs_repair = 0;

    if (home == NULL || home[0] == '\0') {
        return;
    }

    if (snprintf(bin_dir, sizeof(bin_dir), "%s/.gemini/antigravity-cli/bin", home) >=
            (int)sizeof(bin_dir) ||
        access(bin_dir, F_OK) != 0) {
        return;
    }

    if (snprintf(shim_path, sizeof(shim_path), "%s/agentapi", bin_dir) >= (int)sizeof(shim_path)) {
        return;
    }

    if (access(shim_path, F_OK) != 0) {
        needs_repair = 1;
    } else {
        fp = fopen(shim_path, "r");
        if (fp != NULL) {
            size_t read_bytes = fread(buffer, 1, sizeof(buffer) - 1, fp);
            buffer[read_bytes] = '\0';
            (void)fclose(fp);
            if (strstr(buffer, "ld-linux") != NULL) {
                needs_repair = 1;
            }
        }
    }

    if (needs_repair) {
        fp = fopen(shim_path, "w");
        if (fp != NULL) {
            (void)fprintf(fp,
                          "#!%s/bin/sh\n"
                          "exec \"%s/bin/agy\" agentapi \"$@\"\n",
                          prefix, prefix);
            (void)fclose(fp);
            (void)chmod(shim_path, 0755);
        }
    }
}

static void ensure_installed_agentapi(const char *prefix) {
    char target_path[PATH_MAX];
    char bin_path[PATH_MAX];
    FILE *fp = NULL;

    if (snprintf(target_path, sizeof(target_path), "%s/bin/agentapi", prefix) >=
        (int)sizeof(target_path)) {
        return;
    }
    if (snprintf(bin_path, sizeof(bin_path), "%s/bin", prefix) >= (int)sizeof(bin_path)) {
        return;
    }

    if (access(bin_path, F_OK) != 0) {
        return;
    }

    if (access(target_path, X_OK) != 0) {
        fp = fopen(target_path, "w");
        if (fp != NULL) {
            (void)fprintf(fp,
                          "#!%s/bin/sh\n"
                          "exec \"%s/bin/agy\" agentapi \"$@\"\n",
                          prefix, prefix);
            (void)fclose(fp);
            (void)chmod(target_path, 0755);
        }
    }
}

static void wrap_grte_binary_if_needed(const char *binary_path, const char *prefix) {
    char header[4096] = {0};
    FILE *fp = fopen(binary_path, "rb");
    if (fp == NULL) {
        return;
    }
    size_t read_bytes = fread(header, 1, sizeof(header), fp);
    (void)fclose(fp);

    // Only wrap unpatched raw ELF binaries (\x7fELF)
    if (read_bytes < 4 || header[0] != 0x7f || header[1] != 'E' || header[2] != 'L' ||
        header[3] != 'F') {
        return;
    }

    // Verify it is a glibc/GRTE dynamic binary before wrapping
    if (memmem(header, read_bytes, "ld-linux", 8) == NULL &&
        memmem(header, read_bytes, "grte", 4) == NULL) {
        return;
    }

    char real_path[PATH_MAX];
    char tmp_path[PATH_MAX];
    if (snprintf(real_path, sizeof(real_path), "%s.real", binary_path) >= (int)sizeof(real_path) ||
        snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", binary_path, (int)getpid()) >=
            (int)sizeof(tmp_path)) {
        return;
    }

    // Move raw ELF to <path>.real
    if (rename(binary_path, real_path) != 0) {
        return;
    }

    // Write launcher script atomically invoking Termux glibc loader with cleared Bionic preloads
    fp = fopen(tmp_path, "w");
    if (fp == NULL) {
        (void)rename(real_path, binary_path);
        return;
    }

    (void)fprintf(fp,
                  "#!%s/bin/sh\n"
                  "unset LD_PRELOAD\n"
                  "unset LD_LIBRARY_PATH\n"
                  "_target=\"$0.real\"\n"
                  "[ -f \"$_target\" ] || _target=\"$(dirname \"$0\")/$(basename \"$0\").real\"\n"
                  "exec \"%s/glibc/lib/ld-linux-aarch64.so.1\" --library-path \"%s/glibc/lib\" "
                  "\"$_target\" \"$@\"\n",
                  prefix, prefix, prefix);
    (void)fclose(fp);
    (void)chmod(tmp_path, 0755);

    if (rename(tmp_path, binary_path) != 0) {
        (void)unlink(tmp_path);
        (void)rename(real_path, binary_path);
    }
}

static void self_heal_auxiliary_binaries(const char *prefix) {
    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
        home = "/data/data/com.termux/files/home";
    }

    // Self-heal webm_encoder in ~/.gemini/antigravity-cli/bin/
    char webm_path[PATH_MAX];
    if (snprintf(webm_path, sizeof(webm_path), "%s/.gemini/antigravity-cli/bin/webm_encoder",
                 home) < (int)sizeof(webm_path)) {
        wrap_grte_binary_if_needed(webm_path, prefix);
    }
}

static void ensure_termux_shell_bridge(const char *prefix) {
    char dir_path[PATH_MAX];
    char script_path[PATH_MAX];
    char env_script_path[PATH_MAX];
    char bionic_preload[PATH_MAX];
    char real_shell[PATH_MAX];

    (void)snprintf(dir_path, sizeof(dir_path), "%s/libexec/agy", prefix);
    if (access(dir_path, F_OK) != 0) {
        (void)mkdir(dir_path, 0755);
    }

    (void)snprintf(bionic_preload, sizeof(bionic_preload), "%s/lib/libtermux-exec.so", prefix);
    if (access(bionic_preload, R_OK) != 0) {
        return;
    }

    char stat_fix_path[PATH_MAX];
    (void)snprintf(stat_fix_path, sizeof(stat_fix_path), "%s/lib/libtermux-stat-fix.so", prefix);
    if (access(stat_fix_path, R_OK) == 0) {
        (void)snprintf(bionic_preload, sizeof(bionic_preload), "%s/lib/libtermux-exec.so:%s",
                       prefix, stat_fix_path);
    }

    (void)snprintf(script_path, sizeof(script_path), "%s/termux-shell", dir_path);
    (void)snprintf(env_script_path, sizeof(env_script_path), "%s/termux-shell-env", dir_path);

    const char *current_shell = getenv("SHELL");
    if (current_shell == NULL || strstr(current_shell, "termux-shell") != NULL) {
        (void)snprintf(real_shell, sizeof(real_shell), "%s/bin/bash", prefix);
        if (access(real_shell, X_OK) != 0) {
            (void)snprintf(real_shell, sizeof(real_shell), "%s/bin/sh", prefix);
        }
    } else {
        (void)snprintf(real_shell, sizeof(real_shell), "%s", current_shell);
    }
    setenv("AGY_REAL_SHELL", real_shell, 1);

    FILE *fp = fopen(script_path, "w");
    if (fp != NULL) {
        (void)fprintf(fp,
                      "#!/system/bin/sh\n"
                      "export LD_PRELOAD=\"%s\"\n"
                      "export SHELL=\"${AGY_REAL_SHELL:-%s}\"\n"
                      "case \"${ANTIGRAVITY_AGENTAPI_EXE:-}\" in\n"
                      "  *ld-linux*|\"\") export ANTIGRAVITY_AGENTAPI_EXE=\"%s/bin/agy\" ;;\n"
                      "esac\n"
                      "export TMPDIR=\"${TMPDIR:-%s/tmp}\"\n"
                      "export XDG_RUNTIME_DIR=\"${XDG_RUNTIME_DIR:-%s/tmp}\"\n"
                      "exec \"${AGY_REAL_SHELL:-%s}\" \"$@\"\n",
                      bionic_preload, real_shell, prefix, prefix, prefix, real_shell);
        (void)fclose(fp);
        (void)chmod(script_path, 0755);
    }

    FILE *efp = fopen(env_script_path, "w");
    if (efp != NULL) {
        (void)fprintf(
            efp,
            "export LD_PRELOAD=\"%s\"\n"
            "case \"${ANTIGRAVITY_AGENTAPI_EXE:-}\" in\n"
            "  *ld-linux*|\"\") export ANTIGRAVITY_AGENTAPI_EXE=\"%s/bin/agy\" ;;\n"
            "esac\n"
            "export TMPDIR=\"${TMPDIR:-%s/tmp}\"\n"
            "export XDG_RUNTIME_DIR=\"${XDG_RUNTIME_DIR:-%s/tmp}\"\n"
            "_agy_shim=\"${HOME:-/data/data/com.termux/files/home}/.gemini/antigravity-cli/bin/"
            "agentapi\"\n"
            "if [ -f \"$_agy_shim\" ] && grep -q \"ld-linux\" \"$_agy_shim\" 2>/dev/null; then\n"
            "  printf '#!%s/bin/sh\\nexec \"%s/bin/agy\" agentapi \"$@\"\\n' > \"$_agy_shim\" "
            "2>/dev/null || true\n"
            "  chmod 0755 \"$_agy_shim\" 2>/dev/null || true\n"
            "fi\n"
            "agentapi() {\n"
            "  exec \"%s/bin/agy\" agentapi \"$@\"\n"
            "}\n"
            "export -f agentapi 2>/dev/null || true\n",
            bionic_preload, prefix, prefix, prefix, prefix, prefix, prefix);
        (void)fclose(efp);
        (void)chmod(env_script_path, 0644);
    }

    if (access(script_path, X_OK) == 0) {
        setenv("SHELL", script_path, 1);
    }
    if (access(env_script_path, R_OK) == 0) {
        setenv("BASH_ENV", env_script_path, 1);
        setenv("ENV", env_script_path, 1);
    }
}

static void bootstrap_termux_storage(void) {
    const char *storage_root = "/storage/emulated/0";
    if (access(storage_root, R_OK | X_OK) != 0) {
        (void)fprintf(stderr, "[agy-termux] Notice: Storage permission not granted. "
                              "Run 'termux-setup-storage' to access device storage.\n");
        return;
    }

    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
        home = "/data/data/com.termux/files/home";
    }

    char storage_dir[PATH_MAX];
    char shared_link[PATH_MAX];

    if (snprintf(storage_dir, sizeof(storage_dir), "%s/storage", home) >=
            (int)sizeof(storage_dir) ||
        snprintf(shared_link, sizeof(shared_link), "%s/storage/shared", home) >=
            (int)sizeof(shared_link)) {
        return;
    }

    if (access(shared_link, F_OK) != 0) {
        if (access(storage_dir, F_OK) != 0) {
            (void)mkdir(storage_dir, 0755);
        }
        (void)symlink(storage_root, shared_link);
    }
}

static void setup_termux_environment(const char *prefix) {
    char path_buf[PATH_MAX];

    // Ensure UTF-8 locale is configured for glibc.
    if (getenv("LANG") == NULL) {
        setenv("LANG", "en_US.UTF-8", 0);
    }

    // Set dynamic Go resolver.
    setenv("GODEBUG", "netdns=cgo", 1);

    // SSL certificates for Go / curl / OpenSSL.
    if (snprintf(path_buf, sizeof(path_buf), "%s/etc/tls/cert.pem", prefix) <
        (int)sizeof(path_buf)) {
        setenv("SSL_CERT_FILE", path_buf, 1);

        // SSL certificates for Node.js / MCP servers.
        if (getenv("NODE_EXTRA_CA_CERTS") == NULL && access(path_buf, R_OK) == 0) {
            setenv("NODE_EXTRA_CA_CERTS", path_buf, 0);
        }
    }

    // Ensure TMPDIR and XDG_RUNTIME_DIR exist and point to Termux storage (Android /tmp is 0771
    // shell-only).
    if (snprintf(path_buf, sizeof(path_buf), "%s/tmp", prefix) < (int)sizeof(path_buf)) {
        if (access(path_buf, F_OK) != 0) {
            (void)mkdir(path_buf, 0700);
        }
        const char *env_tmp = getenv("TMPDIR");
        if (env_tmp == NULL || env_tmp[0] == '\0') {
            setenv("TMPDIR", path_buf, 1);
        }
        if (getenv("XDG_RUNTIME_DIR") == NULL) {
            setenv("XDG_RUNTIME_DIR", path_buf, 1);
        }
    }

    // Explicitly set ANTIGRAVITY_AGENTAPI_EXE to agy wrapper path before launching the engine.
    if (snprintf(path_buf, sizeof(path_buf), "%s/bin/agy", prefix) < (int)sizeof(path_buf)) {
        setenv("ANTIGRAVITY_AGENTAPI_EXE", path_buf, 1);
    }

    // Allow git discovery across Android FUSE / sdcardfs mount boundaries.
    if (getenv("GIT_DISCOVERY_ACROSS_FILESYSTEM") == NULL) {
        setenv("GIT_DISCOVERY_ACROSS_FILESYSTEM", "1", 0);
    }

    // Default browser handler for Termux OAuth authentication flows.
    if (getenv("BROWSER") == NULL) {
        if (snprintf(path_buf, sizeof(path_buf), "%s/bin/termux-open-url", prefix) <
            (int)sizeof(path_buf)) {
            if (access(path_buf, X_OK) == 0) {
                setenv("BROWSER", path_buf, 0);
            }
        }
    }

    // Ensure localhost host mapping.
    (void)ensure_hosts_config(prefix);

    // Bootstrap Termux storage symlink hierarchy if missing.
    bootstrap_termux_storage();

    // Ensure installed agentapi CLI shim and heal corrupted home shim.
    ensure_installed_agentapi(prefix);
    self_heal_agentapi_shim(prefix);

    // Heal embedded auxiliary GRTE binaries (ripgrep, webm_encoder) for native Termux glibc
    // execution.
    self_heal_auxiliary_binaries(prefix);

    // Bridge subshell execution so child scripts with Unix shebangs work properly.
    ensure_termux_shell_bridge(prefix);
}

static int resolve_qemu_for_cpu(const char *prefix, char *qemu_path, size_t qemu_path_len,
                                const char **qemu) {
    unsigned long hwcap = getauxval(AT_HWCAP);

    *qemu = NULL;
    if ((hwcap & HWCAP_ATOMICS) != 0) {
        return 1;
    }

    int qemu_written = snprintf(qemu_path, qemu_path_len, "%s/bin/qemu-aarch64", prefix);
    if (qemu_written > 0 && (size_t)qemu_written < qemu_path_len && access(qemu_path, F_OK) == 0) {
        *qemu = qemu_path;
        return 1;
    }

    (void)fprintf(stderr, "[agy-termux] CPU lacks LSE atomics, and qemu-aarch64 was not found.\n");
    (void)fprintf(stderr, "[agy-termux] You may need to install the qemu-user-aarch64 package.\n");
    return 0;
}

static int dispatch_update_command(const char *dir, int argc, char **argv,
                                   const char *prefix_path) {
    if (update_command_requests_help(argc, argv)) {
        return handle_update_command(dir, argc, argv);
    }
    if (!require_resolver_config(prefix_path)) {
        return 1;
    }
    return handle_update_command(dir, argc, argv);
}

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static volatile pid_t g_child_pid = 0;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static volatile sig_atomic_t g_child_exited = 0;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static volatile int g_child_exit_status = 0;

static void handle_forward_signal(int sig) {
    // Avoid receiving our own forwarded signal by temporarily ignoring it
    struct sigaction sa_ign;
    memset(&sa_ign, 0, sizeof(sa_ign));
    sa_ign.sa_handler = SIG_IGN;
    sigemptyset(&sa_ign.sa_mask);
    (void)sigaction(sig, &sa_ign, NULL);

    (void)kill(-getpgrp(), sig);

    // Re-arm the forward signal handler
    struct sigaction sa_fwd;
    memset(&sa_fwd, 0, sizeof(sa_fwd));
    sa_fwd.sa_handler = handle_forward_signal;
    sa_fwd.sa_flags = SA_RESTART;
    sigemptyset(&sa_fwd.sa_mask);
    (void)sigaction(sig, &sa_fwd, NULL);
}

static void handle_sigchld(int sig) {
    (void)sig;
    int saved_errno = errno;
    int status = 0;
    pid_t pid = 0;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (pid == g_child_pid) {
            g_child_exited = 1;
            g_child_exit_status = status;
        }
    }
    errno = saved_errno;
}

static void handle_sigwinch(int sig) {
    (void)sig;
    if (g_child_pid > 0) {
        (void)kill(g_child_pid, SIGWINCH);
    }
}

static int supervise_engine_process(const char *exec_target, char **new_argv,
                                    const char *exec_error) {
    // Designate this bootstrapper as a subreaper to adopt orphaned grandchildren.
    (void)prctl(PR_SET_CHILD_SUBREAPER, 1UL, 0UL, 0UL, 0UL);

    // Establish a clean process group and maintain terminal foreground control if interactive.
    if (setpgid(0, 0) == 0 && isatty(STDIN_FILENO)) {
        struct sigaction sa_ttou_ign;
        memset(&sa_ttou_ign, 0, sizeof(sa_ttou_ign));
        sa_ttou_ign.sa_handler = SIG_IGN;
        sigemptyset(&sa_ttou_ign.sa_mask);
        struct sigaction sa_ttou_old;
        memset(&sa_ttou_old, 0, sizeof(sa_ttou_old));
        (void)sigaction(SIGTTOU, &sa_ttou_ign, &sa_ttou_old);
        (void)tcsetpgrp(STDIN_FILENO, getpid());
        (void)sigaction(SIGTTOU, &sa_ttou_old, NULL);
    }

    sigset_t block_mask;
    sigset_t prev_mask;
    sigemptyset(&block_mask);
    sigaddset(&block_mask, SIGCHLD);
    sigaddset(&block_mask, SIGINT);
    sigaddset(&block_mask, SIGTERM);
    sigaddset(&block_mask, SIGHUP);
    sigaddset(&block_mask, SIGQUIT);
    sigprocmask(SIG_BLOCK, &block_mask, &prev_mask);

    struct sigaction sa_fwd;
    memset(&sa_fwd, 0, sizeof(sa_fwd));
    sa_fwd.sa_handler = handle_forward_signal;
    sa_fwd.sa_flags = SA_RESTART;
    sigemptyset(&sa_fwd.sa_mask);
    (void)sigaction(SIGINT, &sa_fwd, NULL);
    (void)sigaction(SIGTERM, &sa_fwd, NULL);
    (void)sigaction(SIGHUP, &sa_fwd, NULL);
    (void)sigaction(SIGQUIT, &sa_fwd, NULL);

    struct sigaction sa_chld;
    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = handle_sigchld;
    sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigemptyset(&sa_chld.sa_mask);
    (void)sigaction(SIGCHLD, &sa_chld, NULL);

    struct sigaction sa_winch;
    memset(&sa_winch, 0, sizeof(sa_winch));
    sa_winch.sa_handler = handle_sigwinch;
    sa_winch.sa_flags = SA_RESTART;
    sigemptyset(&sa_winch.sa_mask);
    (void)sigaction(SIGWINCH, &sa_winch, NULL);

    pid_t parent_pid = getpid();
    pid_t child_pid = fork();
    if (child_pid < 0) {
        perror("[agy-termux] fork failed");
        free(new_argv);
        return 1;
    }

    if (child_pid == 0) {
        (void)signal(SIGINT, SIG_DFL);
        (void)signal(SIGTERM, SIG_DFL);
        (void)signal(SIGHUP, SIG_DFL);
        (void)signal(SIGQUIT, SIG_DFL);
        (void)signal(SIGCHLD, SIG_DFL);
        (void)signal(SIGWINCH, SIG_DFL);

        sigprocmask(SIG_SETMASK, &prev_mask, NULL);

        // Die if parent supervisor exits unexpectedly
        (void)prctl(PR_SET_PDEATHSIG, (unsigned long)SIGTERM, 0UL, 0UL, 0UL);
        if (getppid() != parent_pid) {
            _exit(1);
        }

        // NOLINTNEXTLINE(clang-analyzer-optin.taint.GenericTaint)
        if (execv(exec_target, new_argv) == -1) {
            perror(exec_error);
            free(new_argv);
            _exit(127);
        }
    }

    g_child_pid = child_pid;
    free(new_argv);

    while (!g_child_exited) {
        sigsuspend(&prev_mask);
    }

    // Broadcast SIGTERM to any lingering processes in our process group (e.g. JVM/Node MCP servers)
    (void)kill(-getpgrp(), SIGTERM);

    // Reap all orphaned grandchildren adopted via PR_SET_CHILD_SUBREAPER
    while (waitpid(-1, NULL, WNOHANG) > 0) {
    }

    int exit_code = 0;
    if (WIFEXITED(g_child_exit_status)) {
        exit_code = WEXITSTATUS(g_child_exit_status);
    } else if (WIFSIGNALED(g_child_exit_status)) {
        exit_code = 128 + WTERMSIG(g_child_exit_status);
    }

    return exit_code;
}

int main(int argc, char **argv) {
    char exec_path[PATH_MAX];
    char lib_path[PATH_MAX + 16];
    char patched_bin[PATH_MAX];
    char dynamic_loader[PATH_MAX];
    char prefix_path[PATH_MAX];
    char qemu_path[PATH_MAX];
    char preload_lib[PATH_MAX];
    const char *preload = NULL;
    const char *prefix = getenv("PREFIX");
    const char *loader = NULL;
    const char *dir = NULL;
    const char *qemu = NULL;
    const char *exec_target = NULL;
    const char *exec_error = NULL;
    char **new_argv = NULL;
    int arg_idx = 0;
    int written = 0;
    ssize_t read_len = 0;

    if (!is_native_termux()) {
        print_non_termux_message();
        return 1;
    }

    if (!resolve_qemu_for_cpu(prefix, qemu_path, sizeof(qemu_path), &qemu)) {
        return 1;
    }
    written = snprintf(prefix_path, sizeof(prefix_path), "%s", prefix);
    if (written < 0 || written >= (int)sizeof(prefix_path)) {
        return 1;
    }
    written = snprintf(dynamic_loader, sizeof(dynamic_loader), "%s/glibc/lib/ld-linux-aarch64.so.1",
                       prefix_path);
    if (written < 0 || written >= (int)sizeof(dynamic_loader)) {
        return 1;
    }
    loader = dynamic_loader;
    exec_target = loader;
    exec_error = "[agy-termux] execv failed";

    if (access(loader, F_OK) != 0) {
        (void)fprintf(stderr, "[agy-termux] Missing Termux glibc loader: %s\n", loader);
        (void)fprintf(stderr,
                      "[agy-termux] You may need to install the glibc-repo and glibc packages.\n");
        return 1;
    }

    // Clear conflicting Android Bionic preloads and search paths.
    unsetenv("LD_PRELOAD");
    unsetenv("LD_LIBRARY_PATH");

    // Configure Termux environment variables, certs, temp dir, git discovery, and shell bridge.
    setup_termux_environment(prefix_path);

    read_len = readlink("/proc/self/exe", exec_path, sizeof(exec_path) - 1);
    if (read_len < 0 || read_len >= (ssize_t)sizeof(exec_path)) {
        return 1;
    }
    exec_path[read_len] = '\0';
    dir = dirname(exec_path);

    if (is_update_command(argc, argv)) {
        return dispatch_update_command(dir, argc, argv, prefix_path);
    }

    if (!require_resolver_config(prefix_path)) {
        return 1;
    }

    if (perform_startup_update_check(dir, argc, argv) != 0) {
        (void)execv(exec_path, argv);
    }

    // Use only the Termux glibc runtime libraries.
    written = snprintf(lib_path, sizeof(lib_path), "%s/glibc/lib", prefix_path);
    if (written < 0 || written >= (int)sizeof(lib_path)) {
        return 1;
    }

    // Check for glibc libtermux-exec wrapper for standard path rewriting (/bin/sh, /usr/bin/env).
    written =
        snprintf(preload_lib, sizeof(preload_lib), "%s/glibc/lib/libtermux-exec.so", prefix_path);
    if (written > 0 && written < (int)sizeof(preload_lib) && access(preload_lib, R_OK) == 0) {
        preload = preload_lib;
    }

    // Construct path to the patched binary
    written = snprintf(patched_bin, sizeof(patched_bin), "%s/agy.va39", dir);
    if (written < 0 || written >= (int)sizeof(patched_bin)) {
        return 1;
    }

    // We allocate enough space for: qemu + loader + "--library-path" + lib_path
    // + "--preload" + preload + "--argv0" + "agy" + patched_bin + user args + NULL
    int new_argc = argc + 10;
    new_argv = malloc((size_t)new_argc * sizeof(*new_argv));
    if (!new_argv) {
        return 1;
    }

    arg_idx = 0;
    if (qemu) {
        new_argv[arg_idx++] = (char *)qemu;
        exec_target = qemu;
        exec_error = "[agy-termux] execv (qemu) failed";
    }
    new_argv[arg_idx++] = (char *)loader;
    new_argv[arg_idx++] = "--library-path";
    new_argv[arg_idx++] = lib_path;
    if (preload) {
        new_argv[arg_idx++] = "--preload";
        new_argv[arg_idx++] = (char *)preload;
    }
    new_argv[arg_idx++] = "--argv0";
    new_argv[arg_idx++] = "agy";
    new_argv[arg_idx++] = patched_bin;

    for (int i = 1; i < argc; i++) {
        new_argv[arg_idx++] = argv[i];
    }
    new_argv[arg_idx] = NULL;

    return supervise_engine_process(exec_target, new_argv, exec_error);
}
