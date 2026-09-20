#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/stat.h>
#endif

#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif

// -----------------------------------------------------------------------------
// Path Matching Helpers
// -----------------------------------------------------------------------------

// Matches "/storage/emulated" or "/storage/self" with optional trailing slashes
static inline int is_target_path(const char *pathname) {
    if (pathname == NULL) {
        return 0;
    }
    if (strncmp(pathname, "/storage/emulated", 17) == 0) {
        const char *p = pathname + 17;
        while (*p == '/') {
            p++;
        }
        return *p == '\0';
    }
    if (strncmp(pathname, "/storage/self", 13) == 0) {
        const char *p = pathname + 13;
        while (*p == '/') {
            p++;
        }
        return *p == '\0';
    }
    return 0;
}

// Determines if (dirfd, pathname, flags) evaluates to /storage/emulated or /storage/self
static inline int is_storage_parent_at(int dirfd, const char *pathname, int flags) {
    // 1. Handle AT_EMPTY_PATH on an open file descriptor
    if ((flags & AT_EMPTY_PATH) != 0 && (pathname == NULL || *pathname == '\0')) {
        if (dirfd >= 0) {
            char fd_link[64];
            char target[PATH_MAX];
            (void)snprintf(fd_link, sizeof(fd_link), "/proc/self/fd/%d", dirfd);
            ssize_t len = readlink(fd_link, target, sizeof(target) - 1);
            if (len > 0) {
                target[len] = '\0';
                return is_target_path(target);
            }
        }
        return 0;
    }

    if (pathname == NULL) {
        return 0;
    }

    // 2. Absolute path fast-path (O(1), zero syscalls)
    if (pathname[0] == '/') {
        return is_target_path(pathname);
    }

    // 3. Relative path resolution
    char base_dir[PATH_MAX];
    base_dir[0] = '\0';

    if (dirfd == AT_FDCWD) {
        if (getcwd(base_dir, sizeof(base_dir)) == NULL) {
            return 0;
        }
    } else if (dirfd >= 0) {
        char fd_link[64];
        (void)snprintf(fd_link, sizeof(fd_link), "/proc/self/fd/%d", dirfd);
        ssize_t len = readlink(fd_link, base_dir, sizeof(base_dir) - 1);
        if (len > 0) {
            base_dir[len] = '\0';
        } else {
            return 0;
        }
    } else {
        return 0;
    }

    // If base directory is already /storage/emulated or /storage/self and path is "." or ""
    if (is_target_path(base_dir)) {
        const char *p = pathname;
        while (*p == '.') {
            p++;
        }
        while (*p == '/') {
            p++;
        }
        if (*p == '\0') {
            return 1;
        }
    }

    // If base directory is a direct child (e.g. /storage/emulated/0 or /storage/self/primary)
    // and path is ".." or "../"
    if (strncmp(base_dir, "/storage/emulated/", 18) == 0) {
        const char *rest = base_dir + 18;
        if (strchr(rest, '/') == NULL) {
            const char *p = pathname;
            if (p[0] == '.' && p[1] == '.') {
                p += 2;
                while (*p == '/') {
                    p++;
                }
                if (*p == '\0') {
                    return 1;
                }
            }
        }
    } else if (strncmp(base_dir, "/storage/self/", 14) == 0) {
        const char *rest = base_dir + 14;
        if (strchr(rest, '/') == NULL) {
            const char *p = pathname;
            if (p[0] == '.' && p[1] == '.') {
                p += 2;
                while (*p == '/') {
                    p++;
                }
                if (*p == '\0') {
                    return 1;
                }
            }
        }
    }

    return 0;
}

// -----------------------------------------------------------------------------
// Synthetic Stat Builders
// -----------------------------------------------------------------------------

static void fill_synthetic_stat(struct stat *buf) {
    if (buf == NULL) {
        return;
    }
    memset(buf, 0, sizeof(struct stat));
    buf->st_dev = 1;
    buf->st_ino = 1;
    buf->st_mode = S_IFDIR | 0755;
    buf->st_nlink = 2;
    buf->st_uid = geteuid();
    buf->st_gid = getegid();
    buf->st_rdev = 0;
    buf->st_size = 4096;
    buf->st_blksize = 4096;
    buf->st_blocks = 8;
    time_t now = time(NULL);
    buf->st_atime = now;
    buf->st_mtime = now;
    buf->st_ctime = now;
}

static void fill_synthetic_stat64(struct stat64 *buf) {
    if (buf == NULL) {
        return;
    }
    memset(buf, 0, sizeof(struct stat64));
    buf->st_dev = 1;
    buf->st_ino = 1;
    buf->st_mode = S_IFDIR | 0755;
    buf->st_nlink = 2;
    buf->st_uid = geteuid();
    buf->st_gid = getegid();
    buf->st_rdev = 0;
    buf->st_size = 4096;
    buf->st_blksize = 4096;
    buf->st_blocks = 8;
    time_t now = time(NULL);
    buf->st_atime = now;
    buf->st_mtime = now;
    buf->st_ctime = now;
}

// -----------------------------------------------------------------------------
// Function Pointer Table & Constructor
// -----------------------------------------------------------------------------

typedef int (*real_fstatat_fn)(int dirfd, const char *pathname, struct stat *buf, int flags);
typedef int (*real_fstatat64_fn)(int dirfd, const char *pathname, struct stat64 *buf, int flags);
typedef int (*real_stat_fn)(const char *pathname, struct stat *buf);
typedef int (*real_stat64_fn)(const char *pathname, struct stat64 *buf);
typedef int (*real_lstat_fn)(const char *pathname, struct stat *buf);
typedef int (*real_lstat64_fn)(const char *pathname, struct stat64 *buf);
typedef int (*real_fstat_fn)(int fd, struct stat *buf);
typedef int (*real_fstat64_fn)(int fd, struct stat64 *buf);
typedef int (*real_access_fn)(const char *pathname, int mode);
typedef int (*real_faccessat_fn)(int dirfd, const char *pathname, int mode, int flags);

#if defined(__NR_statx) || defined(STATX_BASIC_STATS)
typedef int (*real_statx_fn)(int dirfd, const char *pathname, int flags, unsigned int mask,
                             struct statx *statxbuf);
static real_statx_fn real_statx = NULL;
#endif

static real_fstatat_fn real_fstatat = NULL;
static real_fstatat64_fn real_fstatat64 = NULL;
static real_stat_fn real_stat = NULL;
static real_stat64_fn real_stat64 = NULL;
static real_lstat_fn real_lstat = NULL;
static real_lstat64_fn real_lstat64 = NULL;
static real_fstat_fn real_fstat = NULL;
static real_fstat64_fn real_fstat64 = NULL;
static real_access_fn real_access = NULL;
static real_faccessat_fn real_faccessat = NULL;

__attribute__((constructor))
static void init_real_functions(void) {
    if (real_fstatat == NULL) {
        real_fstatat = (real_fstatat_fn)dlsym(RTLD_NEXT, "fstatat");
        if (real_fstatat == NULL) {
            real_fstatat = (real_fstatat_fn)dlsym(RTLD_NEXT, "fstatat64");
        }
    }
    if (real_fstatat64 == NULL) {
        real_fstatat64 = (real_fstatat64_fn)dlsym(RTLD_NEXT, "fstatat64");
        if (real_fstatat64 == NULL) {
            real_fstatat64 = (real_fstatat64_fn)dlsym(RTLD_NEXT, "fstatat");
        }
    }
    if (real_stat == NULL) {
        real_stat = (real_stat_fn)dlsym(RTLD_NEXT, "stat");
    }
    if (real_stat64 == NULL) {
        real_stat64 = (real_stat64_fn)dlsym(RTLD_NEXT, "stat64");
        if (real_stat64 == NULL) {
            real_stat64 = (real_stat64_fn)dlsym(RTLD_NEXT, "stat");
        }
    }
    if (real_lstat == NULL) {
        real_lstat = (real_lstat_fn)dlsym(RTLD_NEXT, "lstat");
    }
    if (real_lstat64 == NULL) {
        real_lstat64 = (real_lstat64_fn)dlsym(RTLD_NEXT, "lstat64");
        if (real_lstat64 == NULL) {
            real_lstat64 = (real_lstat64_fn)dlsym(RTLD_NEXT, "lstat");
        }
    }
    if (real_fstat == NULL) {
        real_fstat = (real_fstat_fn)dlsym(RTLD_NEXT, "fstat");
    }
    if (real_fstat64 == NULL) {
        real_fstat64 = (real_fstat64_fn)dlsym(RTLD_NEXT, "fstat64");
        if (real_fstat64 == NULL) {
            real_fstat64 = (real_fstat64_fn)dlsym(RTLD_NEXT, "fstat");
        }
    }
    if (real_access == NULL) {
        real_access = (real_access_fn)dlsym(RTLD_NEXT, "access");
    }
    if (real_faccessat == NULL) {
        real_faccessat = (real_faccessat_fn)dlsym(RTLD_NEXT, "faccessat");
    }
#if defined(__NR_statx) || defined(STATX_BASIC_STATS)
    if (real_statx == NULL) {
        real_statx = (real_statx_fn)dlsym(RTLD_NEXT, "statx");
    }
#endif
}

// -----------------------------------------------------------------------------
// fstatat & fstatat64
// -----------------------------------------------------------------------------

int fstatat(int dirfd, const char *pathname, struct stat *buf, int flags) {
    if (is_storage_parent_at(dirfd, pathname, flags)) {
        if (buf == NULL) {
            errno = EFAULT;
            return -1;
        }
        fill_synthetic_stat(buf);
        return 0;
    }
    if (real_fstatat == NULL) {
        init_real_functions();
    }
    if (real_fstatat == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return real_fstatat(dirfd, pathname, buf, flags);
}

int fstatat64(int dirfd, const char *pathname, struct stat64 *buf, int flags) {
    if (is_storage_parent_at(dirfd, pathname, flags)) {
        if (buf == NULL) {
            errno = EFAULT;
            return -1;
        }
        fill_synthetic_stat64(buf);
        return 0;
    }
    if (real_fstatat64 == NULL) {
        init_real_functions();
    }
    if (real_fstatat64 == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return real_fstatat64(dirfd, pathname, buf, flags);
}

// -----------------------------------------------------------------------------
// stat & stat64
// -----------------------------------------------------------------------------

int stat(const char *pathname, struct stat *buf) {
    if (is_target_path(pathname)) {
        if (buf == NULL) {
            errno = EFAULT;
            return -1;
        }
        fill_synthetic_stat(buf);
        return 0;
    }
    if (real_stat == NULL) {
        init_real_functions();
    }
    if (real_stat == NULL) {
        return fstatat(AT_FDCWD, pathname, buf, 0);
    }
    return real_stat(pathname, buf);
}

int stat64(const char *pathname, struct stat64 *buf) {
    if (is_target_path(pathname)) {
        if (buf == NULL) {
            errno = EFAULT;
            return -1;
        }
        fill_synthetic_stat64(buf);
        return 0;
    }
    if (real_stat64 == NULL) {
        init_real_functions();
    }
    if (real_stat64 == NULL) {
        return fstatat64(AT_FDCWD, pathname, buf, 0);
    }
    return real_stat64(pathname, buf);
}

// -----------------------------------------------------------------------------
// lstat & lstat64
// -----------------------------------------------------------------------------

int lstat(const char *pathname, struct stat *buf) {
    if (is_target_path(pathname)) {
        if (buf == NULL) {
            errno = EFAULT;
            return -1;
        }
        fill_synthetic_stat(buf);
        return 0;
    }
    if (real_lstat == NULL) {
        init_real_functions();
    }
    if (real_lstat == NULL) {
        return fstatat(AT_FDCWD, pathname, buf, AT_SYMLINK_NOFOLLOW);
    }
    return real_lstat(pathname, buf);
}

int lstat64(const char *pathname, struct stat64 *buf) {
    if (is_target_path(pathname)) {
        if (buf == NULL) {
            errno = EFAULT;
            return -1;
        }
        fill_synthetic_stat64(buf);
        return 0;
    }
    if (real_lstat64 == NULL) {
        init_real_functions();
    }
    if (real_lstat64 == NULL) {
        return fstatat64(AT_FDCWD, pathname, buf, AT_SYMLINK_NOFOLLOW);
    }
    return real_lstat64(pathname, buf);
}

// -----------------------------------------------------------------------------
// fstat & fstat64
// -----------------------------------------------------------------------------

int fstat(int fd, struct stat *buf) {
    if (fd >= 0) {
        char fd_link[64];
        char target[PATH_MAX];
        (void)snprintf(fd_link, sizeof(fd_link), "/proc/self/fd/%d", fd);
        ssize_t len = readlink(fd_link, target, sizeof(target) - 1);
        if (len > 0) {
            target[len] = '\0';
            if (is_target_path(target)) {
                if (buf == NULL) {
                    errno = EFAULT;
                    return -1;
                }
                fill_synthetic_stat(buf);
                return 0;
            }
        }
    }
    if (real_fstat == NULL) {
        init_real_functions();
    }
    if (real_fstat == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return real_fstat(fd, buf);
}

int fstat64(int fd, struct stat64 *buf) {
    if (fd >= 0) {
        char fd_link[64];
        char target[PATH_MAX];
        (void)snprintf(fd_link, sizeof(fd_link), "/proc/self/fd/%d", fd);
        ssize_t len = readlink(fd_link, target, sizeof(target) - 1);
        if (len > 0) {
            target[len] = '\0';
            if (is_target_path(target)) {
                if (buf == NULL) {
                    errno = EFAULT;
                    return -1;
                }
                fill_synthetic_stat64(buf);
                return 0;
            }
        }
    }
    if (real_fstat64 == NULL) {
        init_real_functions();
    }
    if (real_fstat64 == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return real_fstat64(fd, buf);
}

// -----------------------------------------------------------------------------
// access, faccessat & faccessat2
// -----------------------------------------------------------------------------

int access(const char *pathname, int mode) {
    if (is_target_path(pathname)) {
        if ((mode & W_OK) != 0) {
            errno = EROFS;
            return -1;
        }
        return 0;
    }
    if (real_access == NULL) {
        init_real_functions();
    }
    if (real_access == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return real_access(pathname, mode);
}

int faccessat(int dirfd, const char *pathname, int mode, int flags) {
    if (is_storage_parent_at(dirfd, pathname, flags)) {
        if ((mode & W_OK) != 0) {
            errno = EROFS;
            return -1;
        }
        return 0;
    }
    if (real_faccessat == NULL) {
        init_real_functions();
    }
    if (real_faccessat == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return real_faccessat(dirfd, pathname, mode, flags);
}

// -----------------------------------------------------------------------------
// statx (Linux 4.11+)
// -----------------------------------------------------------------------------

#if defined(__NR_statx) || defined(STATX_BASIC_STATS)
int statx(int dirfd, const char *pathname, int flags, unsigned int mask, struct statx *statxbuf) {
    if (is_storage_parent_at(dirfd, pathname, flags)) {
        if (statxbuf == NULL) {
            errno = EFAULT;
            return -1;
        }
        memset(statxbuf, 0, sizeof(struct statx));
        statxbuf->stx_mask = STATX_BASIC_STATS;
        statxbuf->stx_mode = S_IFDIR | 0755;
        statxbuf->stx_nlink = 2;
        statxbuf->stx_uid = geteuid();
        statxbuf->stx_gid = getegid();
        statxbuf->stx_size = 4096;
        statxbuf->stx_blksize = 4096;
        statxbuf->stx_blocks = 8;
        statxbuf->stx_ino = 1;
        time_t now = time(NULL);
        statxbuf->stx_atime.tv_sec = now;
        statxbuf->stx_mtime.tv_sec = now;
        statxbuf->stx_ctime.tv_sec = now;
        statxbuf->stx_btime.tv_sec = now;
        return 0;
    }
    if (real_statx == NULL) {
        init_real_functions();
    }
    if (real_statx == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return real_statx(dirfd, pathname, flags, mask, statxbuf);
}
#endif
