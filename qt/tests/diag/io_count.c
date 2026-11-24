// File system call counter for QA runs on macOS: DYLD_INSERT_LIBRARIES=libio_count.dylib.
// Counts calls by operation and by kind of path (previews, metadata databases, catalog, locks, media under
// R237_IO_ROOT, the profile under R237_IO_PROFILE, other), separately for the main thread. The QA driver reads
// the counters through r237_io_snapshot(). Build: clang -dynamiclib -O2 io_count.c -o libio_count.dylib
#include <copyfile.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/attr.h>
#include <sys/clonefile.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#define DYLD_INTERPOSE(_replacement, _replacee)                                                              \
  __attribute__((used)) static struct {                                                                      \
    const void *replacement;                                                                                 \
    const void *replacee;                                                                                    \
  } _interpose_##_replacee __attribute__((section("__DATA,__interpose"))) = {(const void *)&_replacement, \
                                                                             (const void *)&_replacee};

enum { C_THUMB, C_META, C_CATALOG, C_LOCK, C_MEDIA, C_PROFILE, C_OTHER, C_COUNT };
static const char *kCat[C_COUNT] = {"thumb", "metadata", "catalog", "lock", "media", "profile", "other"};
enum { K_OPEN, K_STAT, K_DIR, K_READ, K_WRITE, K_RENAME, K_UNLINK, K_MKDIR, K_FSYNC, K_XATTR, K_COPY, K_FULLSYNC, K_BARRIER, K_COUNT };
static const char *kOp[K_COUNT] = {"open", "stat", "dir", "read", "write", "rename", "unlink", "mkdir", "fsync", "xattr", "copy", "fullsync", "barrier"};
static _Atomic long long counts[2][K_COUNT][C_COUNT];
static _Atomic long long bytesRead[C_COUNT], bytesWritten[C_COUNT];
static _Atomic signed char fdCat[65536];
static char root[1024], profile[1024];
static size_t rootLen, profileLen;
static int ready;

static void init(void) {
  if (ready) return;
  const char *r = getenv("R237_IO_ROOT"), *p = getenv("R237_IO_PROFILE");
  if (r) { strlcpy(root, r, sizeof root); rootLen = strlen(root); }
  if (p) { strlcpy(profile, p, sizeof profile); profileLen = strlen(profile); }
  for (int i = 0; i < 65536; i++) fdCat[i] = C_OTHER;
  ready = 1;
}
static int classify(const char *path) {
  init();
  if (!path) return C_OTHER;
  if (strstr(path, "/.room237-thumb")) return C_THUMB;
  if (strstr(path, ".lock")) return C_LOCK;
  if (strstr(path, "/.room237-metadata") || strstr(path, ".sqlite")) return C_META;
  if (strstr(path, "/.room237-catalog")) return C_CATALOG;
  if (profileLen && strncmp(path, profile, profileLen) == 0) return C_PROFILE;
  if (rootLen && strncmp(path, root, rootLen) == 0) return C_MEDIA;
  if (path[0] != '/') return C_MEDIA;  // Relative to a directory descriptor: the core works inside the root that way.
  return C_OTHER;
}
static int fdClass(int fd) { init(); return fd >= 0 && fd < 65536 ? fdCat[fd] : C_OTHER; }
static void note(int op, int cat) { counts[pthread_main_np() ? 1 : 0][op][cat]++; }
static void bindFd(int fd, int cat) { if (fd >= 0 && fd < 65536) fdCat[fd] = (signed char)cat; }

static int my_open(const char *path, int flags, ...) {
  va_list ap; va_start(ap, flags); int mode = va_arg(ap, int); va_end(ap);
  int c = classify(path); note(K_OPEN, c);
  int fd = open(path, flags, mode); bindFd(fd, c); return fd;
}
DYLD_INTERPOSE(my_open, open)
static int relCat(int dir, const char *path);
static int my_openat(int dir, const char *path, int flags, ...) {
  va_list ap; va_start(ap, flags); int mode = va_arg(ap, int); va_end(ap);
  int c = relCat(dir, path); note(K_OPEN, c);
  int fd = openat(dir, path, flags, mode); bindFd(fd, c); return fd;
}
DYLD_INTERPOSE(my_openat, openat)
static int relCat(int dir, const char *path) {
  if (!path) return C_OTHER;
  if (path[0] == '/') return classify(path);
  if (strstr(path, ".room237-thumb")) return C_THUMB;
  if (strstr(path, ".lock")) return C_LOCK;
  if (strstr(path, ".sqlite") || strstr(path, ".room237-metadata")) return C_META;
  int c = fdClass(dir);
  return c == C_OTHER ? C_MEDIA : c;
}
static int my_stat(const char *p, struct stat *s) { note(K_STAT, classify(p)); return stat(p, s); }
DYLD_INTERPOSE(my_stat, stat)
static int my_lstat(const char *p, struct stat *s) { note(K_STAT, classify(p)); return lstat(p, s); }
DYLD_INTERPOSE(my_lstat, lstat)
static int my_fstat(int fd, struct stat *s) { note(K_STAT, fdClass(fd)); return fstat(fd, s); }
DYLD_INTERPOSE(my_fstat, fstat)
static int my_fstatat(int d, const char *p, struct stat *s, int f) { note(K_STAT, relCat(d, p)); return fstatat(d, p, s, f); }
DYLD_INTERPOSE(my_fstatat, fstatat)
static int my_access(const char *p, int m) { note(K_STAT, classify(p)); return access(p, m); }
DYLD_INTERPOSE(my_access, access)
static int my_faccessat(int d, const char *p, int m, int f) { note(K_STAT, relCat(d, p)); return faccessat(d, p, m, f); }
DYLD_INTERPOSE(my_faccessat, faccessat)
static int my_getattrlist(const char *p, void *a, void *b, size_t n, unsigned int o) { note(K_STAT, classify(p)); return getattrlist(p, a, b, n, o); }
DYLD_INTERPOSE(my_getattrlist, getattrlist)
static int my_fgetattrlist(int fd, void *a, void *b, size_t n, unsigned int o) { note(K_STAT, fdClass(fd)); return fgetattrlist(fd, a, b, n, o); }
DYLD_INTERPOSE(my_fgetattrlist, fgetattrlist)
static int my_getattrlistbulk(int fd, void *a, void *b, size_t n, uint64_t o) { note(K_DIR, fdClass(fd)); return getattrlistbulk(fd, a, b, n, o); }
DYLD_INTERPOSE(my_getattrlistbulk, getattrlistbulk)
static DIR *my_opendir(const char *p) { int c = classify(p); note(K_DIR, c); DIR *d = opendir(p); if (d) bindFd(dirfd(d), c); return d; }
DYLD_INTERPOSE(my_opendir, opendir)
static DIR *my_fdopendir(int fd) { note(K_DIR, fdClass(fd)); return fdopendir(fd); }
DYLD_INTERPOSE(my_fdopendir, fdopendir)
static ssize_t my_read(int fd, void *b, size_t n) { ssize_t r = read(fd, b, n); int c = fdClass(fd); note(K_READ, c); if (r > 0) bytesRead[c] += r; return r; }
DYLD_INTERPOSE(my_read, read)
static ssize_t my_pread(int fd, void *b, size_t n, off_t o) { ssize_t r = pread(fd, b, n, o); int c = fdClass(fd); note(K_READ, c); if (r > 0) bytesRead[c] += r; return r; }
DYLD_INTERPOSE(my_pread, pread)
static ssize_t my_write(int fd, const void *b, size_t n) { ssize_t r = write(fd, b, n); int c = fdClass(fd); if (fd > 2) { note(K_WRITE, c); if (r > 0) bytesWritten[c] += r; } return r; }
DYLD_INTERPOSE(my_write, write)
static ssize_t my_pwrite(int fd, const void *b, size_t n, off_t o) { ssize_t r = pwrite(fd, b, n, o); int c = fdClass(fd); note(K_WRITE, c); if (r > 0) bytesWritten[c] += r; return r; }
DYLD_INTERPOSE(my_pwrite, pwrite)
static int my_rename(const char *a, const char *b) { note(K_RENAME, classify(b)); return rename(a, b); }
DYLD_INTERPOSE(my_rename, rename)
static int my_renameat(int da, const char *a, int db, const char *b) { note(K_RENAME, relCat(db, b)); return renameat(da, a, db, b); }
DYLD_INTERPOSE(my_renameat, renameat)
static int my_renamex_np(const char *a, const char *b, unsigned int f) { note(K_RENAME, classify(b)); return renamex_np(a, b, f); }
DYLD_INTERPOSE(my_renamex_np, renamex_np)
static int my_renameatx_np(int da, const char *a, int db, const char *b, unsigned int f) { note(K_RENAME, relCat(db, b)); return renameatx_np(da, a, db, b, f); }
DYLD_INTERPOSE(my_renameatx_np, renameatx_np)
static int my_unlink(const char *p) { note(K_UNLINK, classify(p)); return unlink(p); }
DYLD_INTERPOSE(my_unlink, unlink)
static int my_unlinkat(int d, const char *p, int f) { note(K_UNLINK, relCat(d, p)); return unlinkat(d, p, f); }
DYLD_INTERPOSE(my_unlinkat, unlinkat)
static int my_mkdir(const char *p, mode_t m) { note(K_MKDIR, classify(p)); return mkdir(p, m); }
DYLD_INTERPOSE(my_mkdir, mkdir)
static int my_mkdirat(int d, const char *p, mode_t m) { note(K_MKDIR, relCat(d, p)); return mkdirat(d, p, m); }
DYLD_INTERPOSE(my_mkdirat, mkdirat)
static int my_rmdir(const char *p) { note(K_UNLINK, classify(p)); return rmdir(p); }
DYLD_INTERPOSE(my_rmdir, rmdir)
static int my_fsync(int fd) { note(K_FSYNC, fdClass(fd)); return fsync(fd); }
DYLD_INTERPOSE(my_fsync, fsync)
static int my_fcntl(int fd, int cmd, ...) {
  va_list ap; va_start(ap, cmd); void *arg = va_arg(ap, void *); va_end(ap);
  if (cmd == F_FULLFSYNC) note(K_FULLSYNC, fdClass(fd));
  if (cmd == F_BARRIERFSYNC) note(K_BARRIER, fdClass(fd));
  int r = fcntl(fd, cmd, arg);
  if ((cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC) && r >= 0) bindFd(r, fdClass(fd));
  return r;
}
DYLD_INTERPOSE(my_fcntl, fcntl)
static int my_close(int fd) { int r = close(fd); bindFd(fd, C_OTHER); return r; }
DYLD_INTERPOSE(my_close, close)
static ssize_t my_getxattr(const char *p, const char *n, void *v, size_t s, u_int32_t pos, int o) { note(K_XATTR, classify(p)); return getxattr(p, n, v, s, pos, o); }
DYLD_INTERPOSE(my_getxattr, getxattr)
static ssize_t my_fgetxattr(int fd, const char *n, void *v, size_t s, u_int32_t pos, int o) { note(K_XATTR, fdClass(fd)); return fgetxattr(fd, n, v, s, pos, o); }
DYLD_INTERPOSE(my_fgetxattr, fgetxattr)
static int my_setxattr(const char *p, const char *n, const void *v, size_t s, u_int32_t pos, int o) { note(K_XATTR, classify(p)); return setxattr(p, n, v, s, pos, o); }
DYLD_INTERPOSE(my_setxattr, setxattr)
static int my_fsetxattr(int fd, const char *n, const void *v, size_t s, u_int32_t pos, int o) { note(K_XATTR, fdClass(fd)); return fsetxattr(fd, n, v, s, pos, o); }
DYLD_INTERPOSE(my_fsetxattr, fsetxattr)
static ssize_t my_listxattr(const char *p, char *b, size_t s, int o) { note(K_XATTR, classify(p)); return listxattr(p, b, s, o); }
DYLD_INTERPOSE(my_listxattr, listxattr)
static ssize_t my_flistxattr(int fd, char *b, size_t s, int o) { note(K_XATTR, fdClass(fd)); return flistxattr(fd, b, s, o); }
DYLD_INTERPOSE(my_flistxattr, flistxattr)
static int my_removexattr(const char *p, const char *n, int o) { note(K_XATTR, classify(p)); return removexattr(p, n, o); }
DYLD_INTERPOSE(my_removexattr, removexattr)
static int my_clonefile(const char *a, const char *b, uint32_t f) { note(K_COPY, classify(b)); return clonefile(a, b, f); }
DYLD_INTERPOSE(my_clonefile, clonefile)
static int my_fclonefileat(int fd, int d, const char *b, uint32_t f) { note(K_COPY, relCat(d, b)); return fclonefileat(fd, d, b, f); }
DYLD_INTERPOSE(my_fclonefileat, fclonefileat)
static int my_copyfile(const char *a, const char *b, copyfile_state_t s, copyfile_flags_t f) { note(K_COPY, classify(b)); return copyfile(a, b, s, f); }
DYLD_INTERPOSE(my_copyfile, copyfile)
static int my_fcopyfile(int a, int b, copyfile_state_t s, copyfile_flags_t f) { note(K_COPY, fdClass(b)); return fcopyfile(a, b, s, f); }
DYLD_INTERPOSE(my_fcopyfile, fcopyfile)

// JSON: {"all":{op:{cat:n}}, "main":{...}, "bytesRead":{cat:n}, "bytesWritten":{cat:n}}. Returns the length.
__attribute__((visibility("default"))) int r237_io_snapshot(char *out, int size, int reset) {
  int n = 0;
#define PUT(...) n += snprintf(out + n, n < size ? size - n : 0, __VA_ARGS__)
  PUT("{");
  for (int t = 0; t < 2; t++) {
    PUT("%s\"%s\":{", t ? "," : "", t ? "main" : "all");
    for (int o = 0; o < K_COUNT; o++) {
      PUT("%s\"%s\":{", o ? "," : "", kOp[o]);
      for (int c = 0; c < C_COUNT; c++) {
        long long v = t ? counts[1][o][c] : counts[0][o][c] + counts[1][o][c];
        PUT("%s\"%s\":%lld", c ? "," : "", kCat[c], v);
      }
      PUT("}");
    }
    PUT("}");
  }
  PUT(",\"bytesRead\":{");
  for (int c = 0; c < C_COUNT; c++) PUT("%s\"%s\":%lld", c ? "," : "", kCat[c], (long long)bytesRead[c]);
  PUT("},\"bytesWritten\":{");
  for (int c = 0; c < C_COUNT; c++) PUT("%s\"%s\":%lld", c ? "," : "", kCat[c], (long long)bytesWritten[c]);
  PUT("}}");
  if (reset) {
    memset((void *)counts, 0, sizeof counts);
    memset((void *)bytesRead, 0, sizeof bytesRead);
    memset((void *)bytesWritten, 0, sizeof bytesWritten);
  }
  return n < size ? n : -1;
}
