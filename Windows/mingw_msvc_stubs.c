// MSVC compatibility shims needed when linking MSVC-built static libs
// (the prebuilt ffmpeg in ffmpeg/Windows10/x64/lib) under mingw-w64.
// MSVC emits references to symbols that aren't in mingw's CRT or libgcc.
// Native-MSVC builds never see this file; gate with __MINGW32__.

#if defined(__MINGW32__) || defined(__MINGW64__)

#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include <emmintrin.h>

// ---------------------------------------------------------------------------
// /GS stack-cookie family. MSVC /GS instruments functions with stack buffers
// to detect overflow. mingw doesn't ship these symbols; no-op stubs are safe
// because we never WROTE the canaries — verifying them is a no-op.
// ---------------------------------------------------------------------------
uintptr_t __security_cookie = 0xBB40E64EUL;

void __security_check_cookie(uintptr_t cookie) {
    (void)cookie;
}

// __GSHandlerCheck is registered as a language-specific SEH handler in the
// .xdata of every MSVC-compiled function with stack cookies. The Windows
// unwinder invokes it on ANY exception passing through such a function,
// not just on actual cookie failures. The signature (per MSVC) is:
//
//   EXCEPTION_DISPOSITION __cdecl __GSHandlerCheck(
//       EXCEPTION_RECORD *, void *, CONTEXT *, DISPATCHER_CONTEXT *);
//
// We just return ExceptionContinueSearch (1) so the unwinder keeps looking
// for an actual handler. Trapping here would kill the process on every
// exception that crosses MSVC-built code, including benign ones.
int __GSHandlerCheck(void *exceptionRecord, void *establisherFrame,
                     void *contextRecord, void *dispatcherContext) {
    (void)exceptionRecord;
    (void)establisherFrame;
    (void)contextRecord;
    (void)dispatcherContext;
    return 1; // ExceptionContinueSearch
}

// MSVC /GS arr-bounds check. Aborts if a stack array index is out of range.
// Reachable only on actual UB; trap is the right fallback.
__attribute__((noreturn))
void __report_rangecheckfailure(void) {
    __builtin_trap();
}

// ---------------------------------------------------------------------------
// __chkstk — MSVC stack-probe (touches each guard page on big stack
// allocations). mingw-w64 ships the equivalent as ___chkstk_ms in libgcc.
// Define a thin tail-jump under the MSVC name. Inline asm rather than
// __attribute__((alias)) because ___chkstk_ms is a libgcc symbol, not a C
// declaration visible at compile time.
// ---------------------------------------------------------------------------
__asm__(
    ".global __chkstk\n"
    "__chkstk:\n"
    "    jmp ___chkstk_ms\n"
);

// ---------------------------------------------------------------------------
// __isa_available — MSVC's CPU-feature dispatch variable. ffmpeg checks it
// (via MSVC's internal dispatcher) to choose code paths. 0 = baseline x87,
// 1 = SSE2, 2 = SSE4.2, 3 = AVX, 4 = AVX2, 5 = AVX-512. We set to SSE4.2
// (safe for x86-64 baseline).
// ---------------------------------------------------------------------------
int __isa_available = 2;

// ---------------------------------------------------------------------------
// Intel SVML (Short Vector Math Library) — MSVC's auto-vectorizer emits
// references to __vdecl_<func><N> when it can pack N scalars into SIMD math
// calls. mingw has no SVML; provide scalar-fallback shims using <math.h>.
// These are correctness-preserving but ~Nx slower than real SVML. ffmpeg
// inner loops calling them will run slower; the codebase still works.
//
// Naming: __vdecl_FOO<N> = vector FOO on N elements
//   pow2/cos2/sin2/exp2/log102/floor2 = 2 doubles  (__m128d)
//   powf4/cosf4/sinf4/expf4           = 4 floats   (__m128)
// ---------------------------------------------------------------------------

__m128d __vdecl_pow2(__m128d x, __m128d y) {
    double xs[2], ys[2], rs[2];
    _mm_storeu_pd(xs, x);
    _mm_storeu_pd(ys, y);
    rs[0] = pow(xs[0], ys[0]);
    rs[1] = pow(xs[1], ys[1]);
    return _mm_loadu_pd(rs);
}

__m128d __vdecl_cos2(__m128d x) {
    double xs[2], rs[2];
    _mm_storeu_pd(xs, x);
    rs[0] = cos(xs[0]);
    rs[1] = cos(xs[1]);
    return _mm_loadu_pd(rs);
}

__m128d __vdecl_sin2(__m128d x) {
    double xs[2], rs[2];
    _mm_storeu_pd(xs, x);
    rs[0] = sin(xs[0]);
    rs[1] = sin(xs[1]);
    return _mm_loadu_pd(rs);
}

__m128d __vdecl_exp2(__m128d x) {
    double xs[2], rs[2];
    _mm_storeu_pd(xs, x);
    rs[0] = exp(xs[0]);
    rs[1] = exp(xs[1]);
    return _mm_loadu_pd(rs);
}

__m128d __vdecl_log102(__m128d x) {
    double xs[2], rs[2];
    _mm_storeu_pd(xs, x);
    rs[0] = log10(xs[0]);
    rs[1] = log10(xs[1]);
    return _mm_loadu_pd(rs);
}

__m128d __vdecl_floor2(__m128d x) {
    double xs[2], rs[2];
    _mm_storeu_pd(xs, x);
    rs[0] = floor(xs[0]);
    rs[1] = floor(xs[1]);
    return _mm_loadu_pd(rs);
}

__m128 __vdecl_powf4(__m128 x, __m128 y) {
    float xs[4], ys[4], rs[4];
    _mm_storeu_ps(xs, x);
    _mm_storeu_ps(ys, y);
    for (int i = 0; i < 4; ++i)
        rs[i] = powf(xs[i], ys[i]);
    return _mm_loadu_ps(rs);
}

__m128 __vdecl_cosf4(__m128 x) {
    float xs[4], rs[4];
    _mm_storeu_ps(xs, x);
    for (int i = 0; i < 4; ++i)
        rs[i] = cosf(xs[i]);
    return _mm_loadu_ps(rs);
}

__m128 __vdecl_sinf4(__m128 x) {
    float xs[4], rs[4];
    _mm_storeu_ps(xs, x);
    for (int i = 0; i < 4; ++i)
        rs[i] = sinf(xs[i]);
    return _mm_loadu_ps(rs);
}

__m128 __vdecl_expf4(__m128 x) {
    float xs[4], rs[4];
    _mm_storeu_ps(xs, x);
    for (int i = 0; i < 4; ++i)
        rs[i] = expf(xs[i]);
    return _mm_loadu_ps(rs);
}

// ---------------------------------------------------------------------------
// MSVC UCRT stdio primitive. ffmpeg's MSVC build inlines snprintf/vsnprintf
// as wrappers that bottom out in `__imp___stdio_common_vsprintf` (the IAT
// entry for the UCRT api-ms-win-crt-stdio DLL). mingw's libmsvcrt.a doesn't
// export this — it has its own snprintf chain that doesn't go through the
// UCRT dispatcher. Define a stub that maps the UCRT options to mingw's
// vsnprintf so the call chain works.
//
// The IAT-style call site is `call *__imp___stdio_common_vsprintf`, so we
// expose this as a function pointer that the linker fills with the address
// of our wrapper. Same trick works for any other __imp_ UCRT primitive.
//
// Sig per MSVC docs:
//   int __stdio_common_vsprintf(uint64_t options, char *buf, size_t n,
//                               const char *fmt, _locale_t locale, va_list);
// We ignore `options` (mostly format flags) and `locale` (always C).
// ---------------------------------------------------------------------------
static int __stdio_common_vsprintf_stub(uint64_t options, char *buf, size_t n,
                                        const char *fmt, void *locale, va_list ap) {
    (void)options;
    (void)locale;
    return vsnprintf(buf, n, fmt, ap);
}

int (*__imp___stdio_common_vsprintf)(uint64_t, char *, size_t, const char *,
                                     void *, va_list) =
    __stdio_common_vsprintf_stub;

// MSVC's __stdio_common_vsscanf is the scanf-family counterpart. Same idea.
static int __stdio_common_vsscanf_stub(uint64_t options, const char *buf,
                                       size_t n, const char *fmt, void *locale,
                                       va_list ap) {
    (void)options;
    (void)n;
    (void)locale;
    return vsscanf(buf, fmt, ap);
}

int (*__imp___stdio_common_vsscanf)(uint64_t, const char *, size_t,
                                    const char *, void *, va_list) =
    __stdio_common_vsscanf_stub;

// stdio shims (snprintf/vsnprintf/sscanf/fprintf) are NOT defined here —
// mingw's <stdio.h> declares those as `inline`, so file-scope redefinitions
// fail to compile. Instead, the mingw toolchain file uses linker --defsym to
// alias the bare symbol names to mingw's __mingw_* variants in libmingwex.

// ---------------------------------------------------------------------------
// MSVC DLL-import-table (IAT) pointers for math functions.
//
// ffmpeg's MSVC build references libc math functions via the UCRT DLL import
// table — every call site is `call qword ptr [__imp_<func>]`, an indirect
// call through an 8-byte memory slot named `__imp_<func>`. When linking
// statically against UCRT, the linker fills those slots with the function
// addresses. Under mingw we don't link UCRT statically, so we provide the
// slots ourselves: 8 bytes of data containing the address of mingw's static
// version of the function (from libmingwex / libucrtbase).
//
// Note: must NOT use --defsym for these — that would alias __imp_foo to foo
// itself, and the indirect call would read foo's code bytes as a pointer.
// We need actual data storage holding the pointer.
// ---------------------------------------------------------------------------

extern long long llrint(double);
extern long long llrintf(float);
extern long lrint(double);
extern long lrintf(float);
extern double rint(double);
extern double round(double);
extern double trunc(double);
extern float truncf(float);
extern double log2(double);
extern float log2f(float);
extern double exp2(double);
extern float exp2f(float);
extern float cbrtf(float);

// MSVC's _dclass / _fdclass aren't in mingw's libmingwex. They live in
// libucrt.a, which we don't link (mingw's snprintf+friends path is via
// libmingwex __mingw_* variants, no need to pull UCRT). Implement here
// using gcc's __builtin_fpclassify so the return values match what the
// MSVC headers' inlined `fpclassify` expects:
//   FP_NAN=2, FP_INFINITE=1, FP_NORMAL=-1, FP_SUBNORMAL=-2, FP_ZERO=0.
int _dclass(double x) {
    return __builtin_fpclassify(2, 1, -1, -2, 0, x);
}
int _fdclass(float x) {
    return __builtin_fpclassify(2, 1, -1, -2, 0, x);
}

void *__imp_llrint   = (void *)&llrint;
void *__imp_llrintf  = (void *)&llrintf;
void *__imp_lrint    = (void *)&lrint;
void *__imp_lrintf   = (void *)&lrintf;
void *__imp_rint     = (void *)&rint;
void *__imp_round    = (void *)&round;
void *__imp_trunc    = (void *)&trunc;
void *__imp_truncf   = (void *)&truncf;
void *__imp_log2     = (void *)&log2;
void *__imp_log2f    = (void *)&log2f;
void *__imp_exp2     = (void *)&exp2;
void *__imp_exp2f    = (void *)&exp2f;
void *__imp_cbrtf    = (void *)&cbrtf;
void *__imp__dclass  = (void *)&_dclass;
void *__imp__fdclass = (void *)&_fdclass;

#endif
