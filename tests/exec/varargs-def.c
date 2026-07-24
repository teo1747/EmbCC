/* Defining a variadic function: va_start builds a SysV __va_list_tag on
 * the frame and forwards it to vsnprintf, which reads int AND double
 * varargs out of the register save area. This is exactly diag_fatal's
 * shape — the last self-hosting blocker. Uses <stdarg.h> so both EmbCC
 * (va_list = char*) and gcc (va_list = __va_list_tag[1]) accept it, and
 * both link the same libc vsnprintf. */
// expect-exit: 42
#include <stdarg.h>
int vsnprintf(char *, unsigned long, const char *, va_list);
static int seq(const char *a, const char *b) {
    int i = 0; while (a[i] && a[i]==b[i]) i++; return a[i]==b[i];
}
static int fmt(char *out, const char *f, ...) {
    va_list ap;
    va_start(ap, f);
    int n = vsnprintf(out, 256, f, ap);
    va_end(ap);
    return n;
}
int main(void) {
    char buf[256];
    fmt(buf, "%d/%s/%d", 7, "mid", 35);
    if (!seq(buf, "7/mid/35")) return 1;
    fmt(buf, "%c%c=%.1f", 'p', 'i', 3.5);   /* double vararg via xmm save */
    if (!seq(buf, "pi=3.5")) return 2;
    int n = fmt(buf, "%d", 12345);
    if (n != 5) return 3;
    return 42;
}
