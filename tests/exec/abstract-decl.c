/* Abstract declarators in casts and sizeof: function-pointer and
 * array-of type names with the name omitted — crt0 casts (void(*)(void))-1
 * when walking .ctors. */
// expect-exit: 42
static int forty(void) { return 40; }
int main(void) {
    int (*fp)(void) = forty;
    /* cast a value to a function-pointer type and back */
    if ((void (*)(void))fp == (void (*)(void))0) return 1;
    /* sizeof an abstract function-pointer type is a pointer's size */
    if (sizeof(int (*)(void)) != sizeof(void *)) return 2;
    /* sizeof an abstract array type */
    if (sizeof(int [4]) != 16) return 3;
    return fp() + 2;   /* 42 */
}
