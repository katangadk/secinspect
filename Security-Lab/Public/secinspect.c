#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <limits.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>

#define MAX_EXTRA 8

int main(int argc, char *argv[]);   /* forward declaration so print_segments() can show its address */

/* Objects that live in different segments (Lecture 4: text/data/BSS/heap/stack) */
int  initialized_global = 42;   /* data segment  */
int  uninitialized_global;      /* BSS segment   */

static void report_error(const char *op, const char *path)
{
    fprintf(stderr, "secinspect: %s failed on '%s': %s (errno=%d)\n",
            op, path, strerror(errno), errno);
}

static const char *file_type(mode_t m)
{
    if (S_ISREG(m))  return "regular file";
    if (S_ISDIR(m))  return "directory";
    if (S_ISCHR(m))  return "character device";
    if (S_ISBLK(m))  return "block device";
    if (S_ISFIFO(m)) return "FIFO/pipe";
    if (S_ISSOCK(m)) return "socket";
    return "unknown"; /* note: stat() follows symlinks, so S_ISLNK won't trigger */
}

static void print_meta(const struct stat *st)
{
    char tbuf[64];
    strftime(tbuf, sizeof tbuf, "%Y-%m-%d %H:%M:%S", localtime(&st->st_mtime));
    printf("  type        : %s\n", file_type(st->st_mode));
    printf("  size        : %lld bytes\n", (long long)st->st_size);
    printf("  permissions : %04o\n", st->st_mode & 07777);
    printf("  owner UID   : %u\n", st->st_uid);
    printf("  group GID   : %u\n", st->st_gid);
    printf("  inode       : %llu\n", (unsigned long long)st->st_ino);
    printf("  hard links  : %lu\n", (unsigned long)st->st_nlink);
    printf("  modified    : %s\n", tbuf);
}

static void show_bytes(const char *buf, ssize_t n)
{
    for (ssize_t i = 0; i < n; i++)
        putchar(buf[i] >= 32 && buf[i] < 127 ? buf[i] : '.');
    putchar('\n');
}

/* What does descriptor <fd> currently refer to? Ask the kernel via /proc. */
static void describe_fd(FILE *out, int fd)
{
    char link[64], target[PATH_MAX];
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    ssize_t r = readlink(link, target, sizeof target - 1);
    if (r < 0 || r == sizeof target - 1) { fprintf(out, "  fd %d -> (not open/truncated)\n", fd); return; }
    target[r] = '\0';
    fprintf(out, "  fd %d -> %s\n", fd, target);
}

/* List every open descriptor of this process (evidence from the OS, not from our source). */
static void list_fds(FILE *out)
{
    DIR *d = opendir("/proc/self/fd");
    if (!d) { fprintf(out, "  cannot open /proc/self/fd: %s\n", strerror(errno)); return; }
    struct dirent *e; int dirfd_num = dirfd(d);
    while ((e = readdir(d)) != NULL) {
        int n = atoi(e->d_name);
        if (n == dirfd_num || e->d_name[0] == '.') continue; /* skip opendir's fd and dots */
        describe_fd(out, n);
    }
    closedir(d);
}

static void print_segments(FILE *out)
{
    int   local_var = 0;                        /* stack */
    void *heap_obj  = malloc(16);               /* heap  */
    fprintf(out, "\n[memory layout - virtual addresses in THIS process]\n");
    fprintf(out, "  text  (code)          main()               = %p\n", (void *)main);
    fprintf(out, "  data  (initialized)   initialized_global   = %p\n", (void *)&initialized_global);
    fprintf(out, "  bss   (uninitialized) uninitialized_global = %p\n", (void *)&uninitialized_global);
    fprintf(out, "  heap  (malloc)        heap_obj             = %p\n", heap_obj);
    fprintf(out, "  stack (local var)     local_var            = %p\n", (void *)&local_var);
    fprintf(out, "  libc  (shared lib)    printf()             = %p\n", (void *)printf);
    fprintf(out, "  Compare with:  cat /proc/%d/maps\n", getpid());
    free(heap_obj);
}

static void usage(const char *p)
{
    fprintf(stderr,
      "Usage: %s [-w text] [-o offset] [-n nbytes] [-e file]... [-l] [-r outfile] [-m] [-p] <file>\n", p);
}

int main(int argc, char *argv[])
{
    const char *wtext = NULL, *redirect_to = NULL;
    const char *extra[MAX_EXTRA]; int n_extra = 0;
    long offset = -1;
    size_t nbytes = 64;
    int do_pause = 0, do_list = 0, do_mem = 0, opt, fd = -1, ret = 0;
    int extra_fd[MAX_EXTRA]; for (int i = 0; i < MAX_EXTRA; i++) extra_fd[i] = -1;
    char *buf = NULL;

    while ((opt = getopt(argc, argv, "w:o:n:e:lr:mp")) != -1) {
        switch (opt) {
        case 'w': wtext = optarg; break;
        case 'o': offset = strtol(optarg, NULL, 10); break;
        case 'n': nbytes = (size_t)strtol(optarg, NULL, 10); if (nbytes <= 0) nbytes = 64; break;
        case 'e': if (n_extra < MAX_EXTRA) extra[n_extra++] = optarg; break;
        case 'l': do_list = 1; break;
        case 'r': redirect_to = optarg; break;
        case 'm': do_mem = 1; break;
        case 'p': do_pause = 1; break;
        default:  usage(argv[0]); return 2;
        }
    }
    if (optind >= argc) { usage(argv[0]); return 2; }
    const char *path = argv[optind];

    /* ---------- Task 1.3: dup2() redirection of stdout ---------- */
    if (redirect_to) {
        fprintf(stderr, "[BEFORE dup2]\n");
        describe_fd(stderr, STDOUT_FILENO);

        int outfd = open(redirect_to, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (outfd == -1) { report_error("open", redirect_to); return 1; }
        fprintf(stderr, "  opened '%s' -> fd %d\n", redirect_to, outfd);

        fflush(stdout);                          /* flush anything buffered for the OLD destination */
        if (dup2(outfd, STDOUT_FILENO) == -1) { report_error("dup2", redirect_to); close(outfd); return 1; }
        close(outfd);                            /* fd 1 keeps the file open; original number no longer needed */

        fprintf(stderr, "[AFTER dup2(%d, 1)]\n", outfd);
        describe_fd(stderr, STDOUT_FILENO);
        fprintf(stderr, "  From now on printf()/putchar() output goes to '%s', not the terminal.\n\n", redirect_to);
    }

    printf("PID: %d\n", getpid());

    /* ---------- Task 1.1: stat() by pathname ---------- */
    struct stat st;
    if (stat(path, &st) == -1) { report_error("stat", path); return 1; }
    printf("\n[stat() on pathname]\n");
    print_meta(&st);

    /* ---------- open() ---------- */
    fd = open(path, wtext ? O_RDWR : O_RDONLY);
    if (fd == -1) { report_error("open", path); return 1; }
    printf("\n[open()] '%s' opened with %s -> file descriptor = %d\n",
           path, wtext ? "O_RDWR" : "O_RDONLY", fd);

    /* ---------- Task 1.2: extra descriptors ---------- */
    for (int i = 0; i < n_extra; i++) {
        extra_fd[i] = open(extra[i], O_RDONLY);
        if (extra_fd[i] == -1) report_error("open", extra[i]);
        else printf("[open()] extra file '%s' -> file descriptor = %d\n", extra[i], extra_fd[i]);
    }

    /* ---------- fstat() by descriptor ---------- */
    struct stat fst;
    if (fstat(fd, &fst) == -1) { report_error("fstat", path); ret = 1; goto cleanup; }
    printf("\n[fstat() on fd %d]\n", fd);
    print_meta(&fst);

    /* ---------- read() ---------- */
    buf = malloc(nbytes + 1);
    if (!buf) { perror("malloc"); ret = 1; goto cleanup; }
    ssize_t n = read(fd, buf, nbytes);
    if (n == -1) { report_error("read", path); ret = 1; goto cleanup; }
    printf("\n[read()] requested %zu bytes, got %zd bytes:\n", nbytes, n);
    show_bytes(buf, n);

    /* ---------- lseek() ---------- */
    off_t pos = lseek(fd, 0, SEEK_CUR);
    printf("[lseek(fd,0,SEEK_CUR)] offset after read = %lld\n", (long long)pos);
    off_t end = lseek(fd, 0, SEEK_END);
    if (end == -1) report_error("lseek(SEEK_END)", path);
    else printf("[lseek(fd,0,SEEK_END)] end of file at offset = %lld\n", (long long)end);

    if (offset >= 0) {
        off_t r = lseek(fd, offset, SEEK_SET);
        if (r == -1) report_error("lseek(SEEK_SET)", path);
        else {
            printf("[lseek(fd,%ld,SEEK_SET)] offset now = %lld\n", offset, (long long)r);
            n = read(fd, buf, nbytes);
            if (n == -1) report_error("read", path);
            else { printf("[read()] %zd bytes from offset %ld:\n", n, offset); show_bytes(buf, n); }
        }
    }

    /* ---------- write() ---------- */
    if (wtext) {
        if (lseek(fd, 0, SEEK_END) == -1) report_error("lseek", path);
        size_t len = strlen(wtext);
        ssize_t w = write(fd, wtext, len);
        if (w == -1) report_error("write", path);
        else printf("\n[write()] wrote %zd of %zu bytes at end of file\n", w, len);
    }

    if (do_list) { printf("\n[open descriptors of this process, per /proc/self/fd]\n"); list_fds(stdout); }
    if (do_mem)  print_segments(stdout);

    if (do_pause) {
        fflush(stdout);
        fprintf(stderr, "\nPaused. PID=%d. In another terminal try:\n"
                        "  ls -l /proc/%d/fd\n  lsof -p %d\n  cat /proc/%d/maps\n"
                        "Press Enter to continue...\n",
                getpid(), getpid(), getpid(), getpid());
        getchar();
    }

cleanup:
    free(buf);
    for (int i = 0; i < n_extra; i++)
        if (extra_fd[i] != -1 && close(extra_fd[i]) == -1) report_error("close", extra[i]);
    if (fd != -1) {
        if (close(fd) == -1) { report_error("close", path); ret = 1; }
        else printf("\n[close()] fd %d closed successfully\n", fd);
    }
    fflush(stdout);
    return ret;
}
