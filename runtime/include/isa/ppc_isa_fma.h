#pragma once
// Fused multiply-add as a host primitive. Gekko's fmadd family and the paired madd family round
// exactly once, so every FMA in the ISA headers goes through these three helpers.
//
// x86-64-v3 and AArch64 have a hardware FMA and std::fma compiles straight to it, so on those
// hosts the helpers are the plain std::fma calls the headers used before.
//
// WebAssembly has no deterministic FMA instruction. Two backends exist:
//   * relaxed SIMD (-mrelaxed-simd): f32x4/f64x2.relaxed_madd. The spec lets an engine compute
//     it fused OR as a separate multiply and add (engines do the latter on x86 CPUs without
//     FMA3), so RuntimeMain calls MkwHostFmaIsFused() at startup and refuses to run when it
//     reports false: an unfused madd would silently break physics parity (ghost sync).
//   * software (default without relaxed SIMD, or forced with MKW_WASM_SOFT_FMA): std::fma
//     lowers to musl's correctly rounded fma/fmaf. Exact everywhere, much slower.

#include <cmath>
#include <cstdint>

#if defined(__wasm__) && defined(__wasm_relaxed_simd__) && !defined(MKW_WASM_SOFT_FMA)
#define MKW_WASM_RELAXED_FMA 1
#endif

#if defined(MKW_WASM_RELAXED_FMA)
using MkwFmaF32x4 = float __attribute__((__vector_size__(16)));
using MkwFmaF64x2 = double __attribute__((__vector_size__(16)));
#endif

#if defined(__wasm__)
// A NaN result's bits are left to the engine on WebAssembly, and musl's software fma computes
// 0 * Inf + NaN as the default NaN where x86 returns the NaN operand. Rewrite NaN results to the
// x86 answer: the first NaN operand, quieted, otherwise the negative default NaN. With two or
// more NaN operands x86 itself has no single answer (it depends on the operand order clang picked
// for each call site), so the fixed a, b, c priority is one of the answers it can give.
inline float MkwResolveFmaNanF32(float a, float b, float c)
{
    for (const float operand : {a, b, c}) {
        if (operand != operand) {
            uint32_t bits = 0;
            __builtin_memcpy(&bits, &operand, sizeof(bits));
            bits |= 0x00400000u;
            float quiet = 0.0f;
            __builtin_memcpy(&quiet, &bits, sizeof(quiet));
            return quiet;
        }
    }
    return __builtin_bit_cast(float, 0xFFC00000u);
}

inline double MkwResolveFmaNanF64(double a, double b, double c)
{
    for (const double operand : {a, b, c}) {
        if (operand != operand) {
            uint64_t bits = 0;
            __builtin_memcpy(&bits, &operand, sizeof(bits));
            bits |= 0x0008000000000000ull;
            double quiet = 0.0;
            __builtin_memcpy(&quiet, &bits, sizeof(quiet));
            return quiet;
        }
    }
    return __builtin_bit_cast(double, 0xFFF8000000000000ull);
}
#endif

#if defined(__wasm__)
// a * b + c rounded once, with whatever NaN bits the backend produces.
inline float MkwRawFmaF32(float a, float b, float c)
{
#if defined(MKW_WASM_RELAXED_FMA)
    const MkwFmaF32x4 va = {a, a, a, a};
    const MkwFmaF32x4 vb = {b, b, b, b};
    const MkwFmaF32x4 vc = {c, c, c, c};
    return __builtin_wasm_relaxed_madd_f32x4(va, vb, vc)[0];
#else
    return std::fma(a, b, c);
#endif
}

inline double MkwRawFmaF64(double a, double b, double c)
{
#if defined(MKW_WASM_RELAXED_FMA)
    const MkwFmaF64x2 va = {a, a};
    const MkwFmaF64x2 vb = {b, b};
    const MkwFmaF64x2 vc = {c, c};
    return __builtin_wasm_relaxed_madd_f64x2(va, vb, vc)[0];
#else
    // musl's fma is exact but slow. When a * b is exact in double, a * b + c rounds only once and
    // equals the FMA. It is exact when one factor is zero, or when the two significands fit in 53
    // bits together and the product is a normal number (the usual case for fmadds, whose factors
    // are a single and a 25-bit value). Counting trailing zeros against an implicit bit
    // overestimates a denormal factor's significand, which only sends it to the slow path.
    if (a == 0.0 || b == 0.0)
        return a * b + c;
    constexpr uint64_t kFraction = 0x000FFFFFFFFFFFFFull;
    constexpr uint64_t kImplicitBit = 1ull << 52;
    const int unusedBits = __builtin_ctzll((__builtin_bit_cast(uint64_t, a) & kFraction) | kImplicitBit) +
                           __builtin_ctzll((__builtin_bit_cast(uint64_t, b) & kFraction) | kImplicitBit);
    if (unusedBits >= 53) {
        const double product = a * b;
        const double magnitude = __builtin_fabs(product);
        if (magnitude >= 0x1p-1021 && magnitude <= __DBL_MAX__)  // false for NaN too
            return product + c;
    }
    return std::fma(a, b, c);
#endif
}
#endif

inline float MkwFusedMulAddF32(float a, float b, float c)
{
#if defined(__wasm__)
    const float result = MkwRawFmaF32(a, b, c);
    if (result != result) [[unlikely]]
        return MkwResolveFmaNanF32(a, b, c);
    return result;
#else
    return std::fma(a, b, c);
#endif
}

inline double MkwFusedMulAddF64(double a, double b, double c)
{
#if defined(__wasm__)
    const double result = MkwRawFmaF64(a, b, c);
    if (result != result) [[unlikely]]
        return MkwResolveFmaNanF64(a, b, c);
    return result;
#else
    return std::fma(a, b, c);
#endif
}

// a * b - c. Kept separate from MkwFusedMulAdd*(a, b, -c) because x86 compiles that form to
// VFMSUB, which returns a sole NaN subtractor with its own sign; negating it first would flip the
// sign of that NaN on hosts that resolve NaNs in software.
inline float MkwFusedMulSubF32(float a, float b, float c)
{
#if defined(__wasm__)
    const float result = MkwRawFmaF32(a, b, -c);
    if (result != result) [[unlikely]]
        return MkwResolveFmaNanF32(a, b, c);
    return result;
#else
    return std::fma(a, b, -c);
#endif
}

inline double MkwFusedMulSubF64(double a, double b, double c)
{
#if defined(__wasm__)
    const double result = MkwRawFmaF64(a, b, -c);
    if (result != result) [[unlikely]]
        return MkwResolveFmaNanF64(a, b, c);
    return result;
#else
    return std::fma(a, b, -c);
#endif
}

#if defined(__wasm__)
// Two float lanes of a paired single; see PpcPairVec in ppc_isa_float.h. NaN lanes are left raw:
// the paired helpers resolve them against the operands they were given (which differ from these
// for msub, where the subtractor arrives negated).
using MkwFmaPair = float __attribute__((__vector_size__(8)));

inline MkwFmaPair MkwFusedMulAddPair(MkwFmaPair a, MkwFmaPair b, MkwFmaPair c)
{
#if defined(MKW_WASM_RELAXED_FMA)
    const MkwFmaF32x4 va = __builtin_shufflevector(a, a, 0, 1, 0, 1);
    const MkwFmaF32x4 vb = __builtin_shufflevector(b, b, 0, 1, 0, 1);
    const MkwFmaF32x4 vc = __builtin_shufflevector(c, c, 0, 1, 0, 1);
    const MkwFmaF32x4 r = __builtin_wasm_relaxed_madd_f32x4(va, vb, vc);
    return __builtin_shufflevector(r, r, 0, 1);
#else
    return MkwFmaPair{std::fma(a[0], b[0], c[0]), std::fma(a[1], b[1], c[1])};
#endif
}
#endif

// True when the helpers above round once. Always true on hosts with a hardware FMA and with the
// software backend; with relaxed SIMD it depends on the engine and CPU, so it is measured. The
// inputs are volatile so the probe exercises the real instruction instead of a folded constant.
// (1 + 2^-23)^2 - (1 + 2^-22) is exactly 2^-46 fused and 0 unfused; likewise 2^-104 for double.
inline bool MkwHostFmaIsFused()
{
    volatile float af = 1.0f + 0x1p-23f;
    volatile float cf = -(1.0f + 0x1p-22f);
    volatile double ad = 1.0 + 0x1p-52;
    volatile double cd = -(1.0 + 0x1p-51);
    bool fused = MkwFusedMulAddF32(af, af, cf) == 0x1p-46f &&
                 MkwFusedMulAddF64(ad, ad, cd) == 0x1p-104;
#if defined(__wasm__)
    const float a = af;
    const float c = cf;
    const MkwFmaPair pair = MkwFusedMulAddPair(MkwFmaPair{a, a}, MkwFmaPair{a, a}, MkwFmaPair{c, c});
    fused = fused && pair[0] == 0x1p-46f && pair[1] == 0x1p-46f;
#endif
    return fused;
}
