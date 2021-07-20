#define F32_MAX FLT_MAX
#define U16_MAX USHRT_MAX;
#define U32_MAX UINT_MAX;
#define U64_MAX ULONG_MAX;
#define I16_MAX SHRT_MAX;
#define I32_MAX INT_MAX;
#define I64_MAX LONG_MAX;

#define ArrayCount(arr) (sizeof((arr)) / (sizeof((arr)[0])))
#define Kilobytes(number) ((number)*1024ull)
#define Megabytes(number) (Kilobytes(number) * 1024ull)
#define Gigabytes(number) (Megabytes(number) * 1024ull)
#define Terabytes(number) (Gigabytes(number) * 1024ull)

#define not !
#define InsideOpen(v, a, b) ((v > a) && (v < b))
#define InsideClosed(v, a, b) ((v >= a) && (v <= b))
#define InsideClosedOpen(v, a, b) ((v >= a) && (v < b))
#define InsideOpenClosed(v, a, b) ((v > a) && (v <= b))
#define InsideUpto InsideClosedOpen
#define InsideDownto InsideOpenClosed
#define InsideExclusive InsideOpen
#define InsideInclusive InsideClosed
#define OutsideExlusive !InsideInclusive
#define OutsideInclusive !InsideExclusive

#ifdef SYNTH_SLOW
    #define Assert(expression) if(!(expression)) { *(int *)0 = 0; }
#else
    #define Assert(expression)
#endif

#define Unreachable Assert(!"Unreachable code")

inline float Log2f(float n) {    return logf( n ) / logf( 2 );  }
/*
static int g_LCG = 4321;
static inline void seed(int* pLCG, int32 seed) { Assert(pLCG != NULL); g_LCG = seed; }
static inline int rand_s32(int* pLCG) { g_LCG = (48271 * g_LCG) % 2147483647; }
static inline unsigned int rand_u32(int* pLCG) { return (unsigned int)rand_s32(pLCG); }
static inline double rand_f64(int* pLCG) { return rand_s32(pLCG) / (double)0x7FFFFFFF; }
static inline float rand_f32(int* pLCG) { return (float)rand_f64(pLCG); }
static inline float scale_to_range_f32(float x, float lo, float hi) { return lo + x * (hi - lo); }
static inline float rand_range_f32(int* pLCG, float lo, float hi) { return scale_to_range_f32(rand_f32(pLCG), lo, hi); }
static inline int rand_range_s32(int* pLCG, int lo, int hi) {
    if (lo == hi) return lo;
    return lo + rand_u32(pLCG) / (0xFFFFFFFF / (hi - lo + 1) + 1);
}
static inline void update_seed(int seed) { seed(&g_LCG, seed); }
static inline int RandomInt(void) { return rand_s32(&g_LCG); }
*/

inline unsigned int RandomU32(unsigned int seed) {
    static unsigned int z = 362436069;
    static unsigned int w = 521288629;
    static unsigned int jcong = 380116160;
    static unsigned int jsr = 123456789;
    unsigned int z_new = 36969 * ((z+seed) & 65535) + ((z + seed) >> 16);
    unsigned int w_new = 18000 * (w & 65535) + (w >> 16);
    unsigned int mwc = (z_new << 16) + w_new;
    unsigned int jcong_new = 69069 * jcong + 1234567;
    unsigned int jsr_new = jsr ^ (jsr << 17);
    jsr_new ^= (jsr >> 13);
    jsr_new ^= (jsr << 5);
    unsigned int result = (mwc ^ jcong_new) + jsr_new;
    z = z_new;
    w = w_new;
    jcong = jcong_new;
    jsr = jsr_new;
    return result;
}

static inline float Randomfloat(unsigned int seed) { return (float)RandomU32(seed) / (float)U32_MAX; }
