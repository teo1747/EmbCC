/* Kernel gap K2: GCC builtins the kernel leans on — byte swaps (network
 * order in net/), a full memory barrier and atomics (spinlocks/ksync), the
 * branch hint, and unreachable. gcc referees the values. */
// expect-exit: 42

static int guarded(int x) {
    if (__builtin_expect(x > 0, 1))     /* hint is discarded; value is x>0 */
        return x + 1;
    __builtin_unreachable();            /* control never falls off here */
}

int main(void) {
    if (__builtin_bswap16(0x1234) != 0x3412) return 1;
    if (__builtin_bswap32(0x11223344u) != 0x44332211u) return 2;
    if (__builtin_bswap64(0x0102030405060708ULL) != 0x0807060504030201ULL) return 3;

    if (guarded(41) != 42) return 4;

    int a = 5;
    if (__atomic_load_n(&a, 5) != 5) return 5;
    __atomic_store_n(&a, 20, 5);
    if (a != 20) return 6;
    int old = __atomic_exchange_n(&a, 100, 5);
    if (old != 20 || a != 100) return 7;

    long b = 7;
    if (__atomic_exchange_n(&b, 9, 5) != 7 || b != 9) return 8;

    __sync_synchronize();
    return 42;
}
