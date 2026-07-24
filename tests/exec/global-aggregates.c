/* File-scope aggregate initializers lowered to a constant .data image:
 * arrays, static arrays, nested struct arrays, wide values, and a char
 * array from a braced list. EmbCC's own predef/keyword/type tables are
 * exactly this shape, so self-hosting needs it. */
// expect-exit: 191
int nums[] = {10, 20, 30};              /* [] size inferred = 3 */
static int sq[3] = {1, 4, 9};
struct pt { int x, y; };
struct pt pts[2] = {{1,2},{3,4}};        /* nested aggregate */
long wide[2] = {0x1122334455667788L, -1L};
char letters[] = {'A','B','C',0};
int main(void){
    int s = 0;
    for (int i=0;i<3;i++) s += nums[i] + sq[i];   /* 74 */
    s += pts[1].x*10 + pts[1].y;                    /* 108 */
    s += (int)(wide[0] >> 56);                      /* 125 */
    s += (int)wide[1];                              /* 124 */
    s += letters[2];                                /* 191 */
    return s & 0xff;
}
