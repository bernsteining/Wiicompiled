#pragma once
// WebAssembly half of ppc_isa_float.h, which includes it at one point (it relies on PpcPairVec,
// the PpcBitCast* helpers, g_mkwHostNiActive and the ppc_isa_fma.h helpers declared before that
// point). Not meant to be included on its own.
//
// WebAssembly differs from the x86-64 and AArch64 hosts in three ways that matter for Gekko's
// floating point, and this file makes it give the x86-64 answers:
//   * no FTZ/DAZ mode: while NI is set, denormal operands and results are flushed in software,
//     with x86's after-rounding tininess rule;
//   * NaN results: the engine chooses their sign and payload, so they are rewritten to what x86
//     produces;
//   * no fused multiply-add instruction (see ppc_isa_fma.h).

#include <cmath>
#include <cstdint>

// --- Scalars, conversions and FMA ---------------------------------------------------------------

inline double PpcHostNiFlushF64(double value) noexcept
{
    const uint64_t bits = PpcBitCastToU64Inline(value);
    if (g_mkwHostNiActive && (bits & 0x7FFFFFFFFFFFFFFFULL) < 0x0010000000000000ULL) [[unlikely]]
        return PpcBitCastToDoubleInline(bits & 0x8000000000000000ULL);
    return value;
}

inline float PpcHostNiFlushF32(float value) noexcept
{
    const uint32_t bits = PpcBitCastToU32Inline(value);
    if (g_mkwHostNiActive && (bits & 0x7FFFFFFFu) < 0x00800000u) [[unlikely]]
        return PpcBitCastToFloatInline(bits & 0x80000000u);
    return value;
}

// float -> double as CVTSS2SD/FCVT do it: NI flushes a denormal input; a NaN keeps its sign and
// payload with the quiet bit forced. WebAssembly's f64.promote_f32 leaves NaN bits to the engine.
inline double PpcHostPromoteInline(float value) noexcept
{
    value = PpcHostNiFlushF32(value);
    const uint32_t bits = PpcBitCastToU32Inline(value);
    if ((bits & 0x7FFFFFFFu) > 0x7F800000u) [[unlikely]]
    {
        return PpcBitCastToDoubleInline((static_cast<uint64_t>(bits & 0x80000000u) << 32) |
                                        0x7FF8000000000000ULL |
                                        (static_cast<uint64_t>(bits & 0x003FFFFFu) << 29));
    }
    return static_cast<double>(value);
}

// double -> float as CVTSD2SS/FCVT do it: NI flushes a denormal input and a denormal result; a
// NaN keeps its sign, its payload is truncated to the top 22 bits and the quiet bit is forced.
// x86 detects tininess after rounding (to 24 bits, exponent unbounded), so under FTZ a result
// whose IEEE rounding lands exactly on FLT_MIN can still be flushed. kPpcHostTinyBound is the
// exact boundary: a single-precision result is tiny iff its exact magnitude is below it (the tie
// at the boundary rounds up to FLT_MIN).
inline constexpr double kPpcHostTinyBound = 0x1.ffffffp-127;  // 2^-126 - 2^-151

inline float PpcHostDemoteInline(double value) noexcept
{
    const uint64_t bits = PpcBitCastToU64Inline(value);
    const uint64_t magnitude = bits & 0x7FFFFFFFFFFFFFFFULL;
    if (magnitude > 0x7FF0000000000000ULL) [[unlikely]]
    {
        return PpcBitCastToFloatInline(static_cast<uint32_t>(
            ((bits >> 32) & 0x80000000u) | 0x7FC00000u | ((bits >> 29) & 0x003FFFFFu)));
    }
    if (g_mkwHostNiActive && PpcBitCastToDoubleInline(magnitude) < kPpcHostTinyBound) [[unlikely]]
        return PpcBitCastToFloatInline(static_cast<uint32_t>((bits >> 32) & 0x80000000u));
    return static_cast<float>(value);
}

// FMA with the host FP environment applied to its operands and result.
template <bool Subtract>
inline double PpcHostFusedF64(double a, double c, double b)
{
    a = PpcHostNiFlushF64(a);
    c = PpcHostNiFlushF64(c);
    b = PpcHostNiFlushF64(b);
    const double result = Subtract ? MkwFusedMulSubF64(a, c, b) : MkwFusedMulAddF64(a, c, b);
    // x86's after-rounding tininess at double precision (see kPpcHostTinyBound): a result that
    // IEEE-rounded to exactly +-DBL_MIN is flushed when rounding the exact value to 53 bits with an
    // unbounded exponent stays below DBL_MIN. Scaling the operands by powers of two is exact and
    // moves that rounding out of the denormal range; overflowing operands keep the IEEE result.
    if (g_mkwHostNiActive &&
        (PpcBitCastToU64Inline(result) & 0x7FFFFFFFFFFFFFFFULL) == 0x0010000000000000ULL) [[unlikely]]
    {
        const double scaledA = a * 0x1p64;
        const double scaledC = c * 0x1p64;
        const double scaledB = b * 0x1p128;
        if (std::isfinite(scaledA) && std::isfinite(scaledC) && std::isfinite(scaledB))
        {
            const double scaled = Subtract ? MkwFusedMulSubF64(scaledA, scaledC, scaledB)
                                           : MkwFusedMulAddF64(scaledA, scaledC, scaledB);
            if (std::fabs(scaled) < 0x1p-894)
                return PpcBitCastToDoubleInline(PpcBitCastToU64Inline(result) & 0x8000000000000000ULL);
        }
    }
    return PpcHostNiFlushF64(result);
}

template <bool Subtract>
inline float PpcHostFusedF32(float a, float c, float b)
{
    a = PpcHostNiFlushF32(a);
    c = PpcHostNiFlushF32(c);
    b = PpcHostNiFlushF32(b);
    return PpcHostNiFlushF32(Subtract ? MkwFusedMulSubF32(a, c, b) : MkwFusedMulAddF32(a, c, b));
}

// --- Paired singles -----------------------------------------------------------------------------
// NaN result lanes are rewritten to the x86 answer, as the AArch64 branch of ppc_isa_float.h does
// for NEON.

inline PpcPairBits PpcPairNanMaskInline(PpcPairVec value)
{
    return __builtin_bit_cast(PpcPairBits, value != value);
}

inline uint64_t PpcPairNanLaneBitsInline(PpcPairVec value)
{
    return __builtin_bit_cast(uint64_t, PpcPairNanMaskInline(value));
}

inline PpcPairVec PpcPairSelectInline(PpcPairBits mask, PpcPairVec ifSet, PpcPairVec ifClear)
{
    return __builtin_bit_cast(PpcPairVec,
        (__builtin_bit_cast(PpcPairBits, ifSet) & mask) |
        (__builtin_bit_cast(PpcPairBits, ifClear) & ~mask));
}

inline PpcPairVec PpcQuietPairInline(PpcPairVec value)
{
    return __builtin_bit_cast(PpcPairVec, __builtin_bit_cast(PpcPairBits, value) | 0x00400000u);
}

inline PpcPairVec PpcDefaultNanPairInline()
{
    return __builtin_bit_cast(PpcPairVec, PpcPairBits{0xFFC00000u, 0xFFC00000u});
}

// The first NaN operand, quieted, otherwise the default NaN (MkwResolveFmaNanF32 per lane).
inline PpcPairVec PpcResolveNanLanes3Inline(
    PpcPairVec result, PpcPairVec op1, PpcPairVec op2, PpcPairVec op3)
{
    PpcPairVec replacement = PpcDefaultNanPairInline();
    replacement = PpcPairSelectInline(PpcPairNanMaskInline(op3), PpcQuietPairInline(op3), replacement);
    replacement = PpcPairSelectInline(PpcPairNanMaskInline(op2), PpcQuietPairInline(op2), replacement);
    replacement = PpcPairSelectInline(PpcPairNanMaskInline(op1), PpcQuietPairInline(op1), replacement);
    return PpcPairSelectInline(PpcPairNanMaskInline(result), replacement, result);
}

inline PpcPairVec PpcResolveNanLanesInline(PpcPairVec result, PpcPairVec op1, PpcPairVec op2)
{
    return PpcResolveNanLanes3Inline(result, op1, op2, op2);
}

// What MXCSR.FTZ|DAZ (FPCR.FZ) do in hardware on the other hosts while NI is set: a denormal lane
// is read, and written, as a zero of the same sign. Zero, normal, Inf and NaN lanes pass through.
inline PpcPairVec PpcWasmFlushPairInline(PpcPairVec value)
{
    const PpcPairBits bits = __builtin_bit_cast(PpcPairBits, value);
    const PpcPairBits tiny = __builtin_bit_cast(PpcPairBits, (bits & 0x7FFFFFFFu) < 0x00800000u);
    return __builtin_bit_cast(PpcPairVec, bits & (~tiny | 0x80000000u));
}

// FTZ on a result pair, with x86's after-rounding tininess: a lane IEEE-rounded to exactly
// +-FLT_MIN is still flushed when its exact value (exactLane(i), computed in double) is below
// kPpcHostTinyBound. Only reached while NI is set.
template <typename ExactLane>
inline PpcPairVec PpcWasmFtzPairInline(PpcPairVec result, ExactLane exactLane)
{
    const PpcPairBits bits = __builtin_bit_cast(PpcPairBits, result);
    const PpcPairBits atFloatMin = __builtin_bit_cast(PpcPairBits, (bits & 0x7FFFFFFFu) == 0x00800000u);
    if (__builtin_bit_cast(uint64_t, atFloatMin) != 0) [[unlikely]]
    {
        for (int lane = 0; lane < 2; ++lane)
        {
            if (atFloatMin[lane] && std::fabs(exactLane(lane)) < kPpcHostTinyBound)
                result[lane] = PpcBitCastToFloatInline(bits[lane] & 0x80000000u);
        }
    }
    return PpcWasmFlushPairInline(result);
}

// `operation` is generic so the same expression also yields the exact double result per lane.
template <typename Operation>
inline PpcPairVec PpcWasmPairArithmeticInline(PpcPairVec lhs, PpcPairVec rhs, Operation operation)
{
    const bool ni = g_mkwHostNiActive;
    if (ni) [[unlikely]]
    {
        lhs = PpcWasmFlushPairInline(lhs);
        rhs = PpcWasmFlushPairInline(rhs);
    }
    PpcPairVec result = operation(lhs, rhs);
    if (PpcPairNanLaneBitsInline(result) != 0) [[unlikely]]
        result = PpcResolveNanLanesInline(result, lhs, rhs);
    if (ni) [[unlikely]]
    {
        result = PpcWasmFtzPairInline(result, [&](int lane) {
            return operation(static_cast<double>(lhs[lane]), static_cast<double>(rhs[lane]));
        });
    }
    return result;
}

// multiplicand * multiplier + addend (or - addend), rounded once per lane. As on AArch64 the NaN
// resolver sees the original subtractor, because negating it would flip a sole NaN's sign.
template <bool Subtract>
inline PpcPairVec PpcWasmPairFusedInline(
    PpcPairVec multiplicand, PpcPairVec multiplier, PpcPairVec addend)
{
    const bool ni = g_mkwHostNiActive;
    if (ni) [[unlikely]]
    {
        multiplicand = PpcWasmFlushPairInline(multiplicand);
        multiplier = PpcWasmFlushPairInline(multiplier);
        addend = PpcWasmFlushPairInline(addend);
    }
    PpcPairVec result = MkwFusedMulAddPair(multiplicand, multiplier, Subtract ? -addend : addend);
    if (PpcPairNanLaneBitsInline(result) != 0) [[unlikely]]
        result = PpcResolveNanLanes3Inline(result, multiplicand, multiplier, addend);
    if (ni) [[unlikely]]
    {
        result = PpcWasmFtzPairInline(result, [&](int lane) {
            const double c = static_cast<double>(addend[lane]);
            return MkwFusedMulAddF64(static_cast<double>(multiplicand[lane]),
                                     static_cast<double>(multiplier[lane]), Subtract ? -c : c);
        });
    }
    return result;
}
