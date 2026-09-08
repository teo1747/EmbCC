// expect-exit: 42
int printf(const char*,...);
static long suma(int*a,int n){long s=0;for(int i=0;i<n;i++)s+=a[i];return s;}
static void filla(int*a,int n,int b){for(int i=0;i<n;i++)a[i]=b+i;}
/* SAME scope, disjoint liveness: a1 dead before a2 used -> gcc/embcc share slot.
 * Both address-taken; the sound rule keeps them scope-bounded, so this is the
 * disjoint-scope... no: they are same-scope. This tests the AT path stays SAFE. */
static long same_scope_at(void){
  int a1[64]; filla(a1,64,100); long r = suma(a1,64);   /* a1 used, then dead */
  int a2[64]; filla(a2,64,200); r += suma(a2,64);        /* a2 after a1 */
  return r;  /* 8416 + 14816 = 23232 */
}
/* non-address-taken scalars, same scope, disjoint liveness -> coalesce by liveness */
static long scalars(int x){
  int p = x*2; int q = p+1;             /* p,q live early */
  int u = x*3; int v = u+1;             /* u,v live later; p,q dead */
  return (long)q + v;                    /* (2x+1) + (3x+1) = 5x+2 */
}
/* address-taken with OVERLAPPING liveness must NOT corrupt (both live at once) */
static long both_live(int x){
  int b1[8], b2[8];
  filla(b1,8,x); filla(b2,8,x*10);
  return suma(b1,8) + suma(b2,8);        /* both needed together */
}
int main(void){
  if (same_scope_at() != 23232) return 1;
  if (scalars(10) != 52) return 2;       /* 5*10+2 */
  /* both_live(3): b1=3..10 sum=52; b2=30..37 sum=268; total 320 */
  if (both_live(3) != 320) return 3;
  return 42;
}
