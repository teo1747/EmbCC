/* The syscall floor for the aarch64 test harness: newlib's bare POSIX
 * names implemented over ARM semihosting.
 *
 * Why this file exists rather than -lrdimon: the aarch64 newlib in
 * ~/cross was configured for EmbLinkOS's userland, which supplies the
 * BARE names (write, read, ...) from user/lib/syscalls.c. Stock librdimon
 * defines the underscore-prefixed ones, so it does not resolve against
 * this libc. This is the same layer EmbLinkOS's syscalls.c is, with
 * `hlt #0xf000` where the OS has `svc #0`.
 *
 * The console is the one real device: fds 0/1/2 all bind to the ":tt"
 * semihosting stream. Everything else is the minimum newlib needs to
 * start and to run printf/malloc.
 */
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define SYS_OPEN   0x01
#define SYS_CLOSE  0x02
#define SYS_WRITE  0x05
#define SYS_READ   0x06
#define SYS_ISTTY  0x09
#define SYS_SEEK   0x0A
#define SYS_FLEN   0x0C
#define SYS_EXIT   0x18

/* ADP_Stopped_ApplicationExit: the reason code QEMU turns into its own
 * process exit status, which is how a test's `// expect-exit: N` is read
 * back on the host. */
#define ADP_STOPPED_APPLICATION_EXIT 0x20026

static long semihost(long op, void *arg)
{
    register long x0 __asm__("x0") = op;
    register void *x1 __asm__("x1") = arg;
    __asm__ volatile("hlt #0xf000"
                     : "+r"(x0)
                     : "r"(x1)
                     : "memory");
    return x0;
}

/* The ":tt" handle, opened on first use. -1 = not yet, -2 = open failed
 * (output is then dropped rather than trapping on every character). */
static int tt = -1;

static int tt_handle(void)
{
    if (tt == -1) {
        static const char name[] = ":tt";
        long blk[3];
        blk[0] = (long)(unsigned long)name;
        blk[1] = 4;                 /* mode "w" */
        blk[2] = (long)(sizeof name - 1);
        long h = semihost(SYS_OPEN, blk);
        tt = (h < 0) ? -2 : (int)h;
    }
    return tt;
}

/* newlib's _READ_WRITE_RETURN_TYPE is plain int in this build, so these
 * two must be int and not ssize_t or the declarations conflict. */
int write(int fd, const void *buf, size_t len)
{
    (void)fd;                        /* stdout and stderr share the console */
    int h = tt_handle();
    if (h < 0)
        return (int)len;             /* no console: succeed silently */
    long blk[3];
    blk[0] = h;
    blk[1] = (long)(unsigned long)buf;
    blk[2] = (long)len;
    /* SYS_WRITE returns the number of bytes NOT written. */
    long left = semihost(SYS_WRITE, blk);
    if (left < 0 || (size_t)left > len)
        return -1;
    return (int)(len - (size_t)left);
}

int read(int fd, void *buf, size_t len)
{
    (void)fd; (void)buf; (void)len;
    return 0;                        /* stdin is always at EOF */
}

int close(int fd)
{
    (void)fd;
    return 0;
}

off_t lseek(int fd, off_t off, int whence)
{
    (void)fd; (void)off; (void)whence;
    errno = ESPIPE;                  /* the console is not seekable */
    return (off_t)-1;
}

int fstat(int fd, struct stat *st)
{
    (void)fd;
    st->st_mode = S_IFCHR;           /* character device: newlib leaves
                                      * stdout unbuffered, so output
                                      * survives a test that aborts */
    st->st_blksize = 1024;
    return 0;
}

int isatty(int fd)
{
    (void)fd;
    return 1;
}

int getpid(void)
{
    return 1;
}

int kill(int pid, int sig)
{
    (void)pid; (void)sig;
    errno = EINVAL;
    return -1;
}

/* The heap is the arena the linker script reserved; there is no MMU
 * involvement and no growth beyond it. */
extern char __heap_start[], __heap_end[];

void *sbrk(ptrdiff_t incr)
{
    static char *brk;
    if (brk == 0)
        brk = __heap_start;
    if (incr < 0 || (size_t)(__heap_end - brk) < (size_t)incr) {
        errno = ENOMEM;
        return (void *)-1;
    }
    char *prev = brk;
    brk += incr;
    return prev;
}

void _exit(int code)
{
    long blk[2];
    blk[0] = ADP_STOPPED_APPLICATION_EXIT;
    blk[1] = code;
    semihost(SYS_EXIT, blk);
    for (;;)
        ;
}
