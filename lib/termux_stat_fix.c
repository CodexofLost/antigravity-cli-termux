#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/stat.h>
#endif

static const char *get_synthetic_emulated_dir(void) {
    static char synth_dir[PATH_MAX] = {0};
    if (synth_dir[0] == '\0') {
        const char *prefix = getenv("PREFIX");
        if (prefix == NULL || prefix[0] == '\0') {
            prefix = "/data/data/com.termux/files/usr";
        }
        if (snprintf(synth_dir, sizeof(synth_dir), "%s/tmp/.synthetic_emulated", prefix) <
            (int)sizeof(synth_dir)) {
            (void)mkdir(synth_dir, 0755);
            char link_path[PATH_MAX];
            if (snprintf(link_path, sizeof(link_path), "%s/0", synth_dir) <
                (int)sizeof(link_path)) {
                struct stat st;
                if (lstat(link_path, &st) != 0) {
                    (void)symlink("/storage/emulated/0", link_path);
                }
            }
        }
    }
    return synth_dir;
}

// Matches "/storage/emulated", "/storage/emulated/", or paths with multiple trailing slashes
static inline int is_emulated_storage_parent(const char *pathname) {
    if (pathname == NULL || strncmp(pathname, "/storage/emulated", 17) != 0) {
        return 0;
    }
    const char *p = pathname + 17;
    while (*p == '/') {
        p++;
    }
    return *p == '\0';
}

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
// fstatat & fstatat64
// -----------------------------------------------------------------------------
typedef int (*real_fstatat_fn)(int dirfd, const char *pathname, struct stat *buf, int flags);
typedef int (*real_fstatat64_fn)(int dirfd, const char *pathname, struct stat64 *buf, int flags);

int fstatat(int dirfd, const char *pathname, struct stat *buf, int flags) {
    if (is_emulated_storage_parent(pathname)) {
        if (buf == NULL) {
            errno = EFAULT;
            return -1;
        }
        fill_synthetic_stat(buf);
        return 0;
    }
    static real_fstatat_fn real_fn = NULL;
    if (real_fn == NULL) {
        real_fn = (real_fstatat_fn)dlsym(RTLD_NEXT, "fstatat");
        if (real_fn == NULL) {
            real_fn = (real_fstatat_fn)dlsym(RTLD_NEXT, "fstatat64");
        }
    }
    if (real_fn == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return real_fn(dirfd, pathname, buf, flags);
}

int fstatat64(int dirfd, const char *pathname, struct stat64 *buf, int flags) {
    if (is_emulated_storage_parent(pathname)) {
        if (buf == NULL) {
            errno = EFAULT;
            return -1;
        }
        fill_synthetic_stat64(buf);
        return 0;
    }
    static real_fstatat64_fn real_fn = NULL;
    if (real_fn == NULL) {
        real_fn = (real_fstatat64_fn)dlsym(RTLD_NEXT, "fstatat64");
        if (real_fn == NULL) {
            real_fn = (real_fstatat64_fn)dlsym(RTLD_NEXT, "fstatat");
        }
    }
    if (real_fn == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return real_fn(dirfd, pathname, buf, flags);
}

// -----------------------------------------------------------------------------
// stat & stat64
// -----------------------------------------------------------------------------
typedef int (*real_stat_fn)(const char *pathname, struct stat *buf);
typedef int (*real_stat64_fn)(const char *pathname, struct stat64 *buf);

int stat(const char *pathname, struct stat *buf) {
    if (is_emulated_storage_parent(pathname)) {
        if (buf == NULL) {
            errno = EFAULT;
            return -1;
        }
        fill_synthetic_stat(buf);
        return 0;
    }
    static real_stat_fn real_fn = NULL;
    if (real_fn == NULL) {
        real_fn = (real_stat_fn)dlsym(RTLD_NEXT, "stat");
    }
    if (real_fn == NULL) {
        return fstatat(AT_FDCWD, pathname, buf, 0);
    }
    return real_fn(pathname, buf);
}

int stat64(const char *pathname, struct stat64 *buf) {
    if (is_emulated_storage_parent(pathname)) {
        if (buf == NULL) {
            errno = EFAULT;
            return -1;
        }
        fill_synthetic_stat64(buf);
        return 0;
    }
    static real_stat64_fn real_fn = NULL;
    if (real_fn == NULL) {
        real_fn = (real_stat64_fn)dlsym(RTLD_NEXT, "stat64");
        if (real_fn == NULL) {
            real_fn = (real_stat64_fn)dlsym(RTLD_NEXT, "stat");
        }
    }
    if (real_fn == NULL) {
        return fstatat64(AT_FDCWD, pathname, buf, 0);
    }
    return real_fn(pathname, buf);
}

// -----------------------------------------------------------------------------
// lstat & lstat64
// -----------------------------------------------------------------------------
int lstat(const char *pathname, struct stat *buf) {
    if (is_emulated_storage_parent(pathname)) {
        if (buf == NULL) {
            errno = EFAULT;
            return -1;
        }
        fill_synthetic_stat(buf);
        return 0;
    }
    static real_stat_fn real_fn = NULL;
    if (real_fn == NULL) {
        real_fn = (real_stat_fn)dlsym(RTLD_NEXT, "lstat");
    }
    if (real_fn == NULL) {
        return fstatat(AT_FDCWD, pathname, buf, AT_SYMLINK_NOFOLLOW);
    }
    return real_fn(pathname, buf);
}

int lstat64(const char *pathname, struct stat64 *buf) {
    if (is_emulated_storage_parent(pathname)) {
        if (buf == NULL) {
            errno = EFAULT;
            return -1;
        }
        fill_synthetic_stat64(buf);
        return 0;
    }
    static real_stat64_fn real_fn = NULL;
    if (real_fn == NULL) {
        real_fn = (real_stat64_fn)dlsym(RTLD_NEXT, "lstat64");
        if (real_fn == NULL) {
            real_fn = (real_stat64_fn)dlsym(RTLD_NEXT, "lstat");
        }
    }
    if (real_fn == NULL) {
        return fstatat64(AT_FDCWD, pathname, buf, AT_SYMLINK_NOFOLLOW);
    }
    return real_fn(pathname, buf);
}

// -----------------------------------------------------------------------------
// statx (Linux 4.11+)
// -----------------------------------------------------------------------------
#if defined(__NR_statx) || defined(STATX_BASIC_STATS)
typedef int (*real_statx_fn)(int dirfd, const char *pathname, int flags, unsigned int mask,
                             struct statx *statxbuf);

int statx(int dirfd, const char *pathname, int flags, unsigned int mask, struct statx *statxbuf) {
    if (is_emulated_storage_parent(pathname)) {
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
    static real_statx_fn real_fn = NULL;
    if (real_fn == NULL) {
        real_fn = (real_statx_fn)dlsym(RTLD_NEXT, "statx");
    }
    if (real_fn == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return real_fn(dirfd, pathname, flags, mask, statxbuf);
}
#endif

// -----------------------------------------------------------------------------
// access & faccessat
// -----------------------------------------------------------------------------
typedef int (*real_access_fn)(const char *pathname, int mode);
typedef int (*real_faccessat_fn)(int dirfd, const char *pathname, int mode, int flags);

int access(const char *pathname, int mode) {
    if (is_emulated_storage_parent(pathname)) {
        if ((mode & W_OK) != 0) {
            errno = EACCES;
            return -1;
        }
        return 0;
    }
    static real_access_fn real_fn = NULL;
    if (real_fn == NULL) {
        real_fn = (real_access_fn)dlsym(RTLD_NEXT, "access");
    }
    if (real_fn == NULL) {
        return faccessat(AT_FDCWD, pathname, mode, 0);
    }
    return real_fn(pathname, mode);
}

int faccessat(int dirfd, const char *pathname, int mode, int flags) {
    if (is_emulated_storage_parent(pathname)) {
        if ((mode & W_OK) != 0) {
            errno = EACCES;
            return -1;
        }
        return 0;
    }
    static real_faccessat_fn real_fn = NULL;
    if (real_fn == NULL) {
        real_fn = (real_faccessat_fn)dlsym(RTLD_NEXT, "faccessat");
    }
    if (real_fn == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return real_fn(dirfd, pathname, mode, flags);
}

// -----------------------------------------------------------------------------
// opendir
// -----------------------------------------------------------------------------
typedef DIR *(*real_opendir_fn)(const char *name);

DIR *opendir(const char *name) {
    if (is_emulated_storage_parent(name)) {
        name = get_synthetic_emulated_dir();
    }
    static real_opendir_fn real_fn = NULL;
    if (real_fn == NULL) {
        real_fn = (real_opendir_fn)dlsym(RTLD_NEXT, "opendir");
    }
    if (real_fn == NULL) {
        errno = ENOSYS;
        return NULL;
    }
    return real_fn(name);
}
