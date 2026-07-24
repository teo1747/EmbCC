/* A function with more than 6 integer parameters: SysV passes the 7th
 * onward on the stack, and the callee must read them from [rbp+16...],
 * not from nonexistent registers. EmbCC's own codegen_unit takes ten
 * parameters, so self-hosting depends on this. */
// expect-exit: 42
static int pick(int a, int b, int c, int d, int e, int f, int g, int h) {
    return a + b + c + d + e + f + g + h;
}
int main(void) {
    /* 1+2+3+4+5+6+7+14 = 42; g and h are the stack-passed ones */
    return pick(1, 2, 3, 4, 5, 6, 7, 14);
}
