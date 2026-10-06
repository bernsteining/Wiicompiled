// Differential test for the floating-point ISA helpers.
//
// Every scalar and paired-single helper is run over edge-case and pseudo-random inputs, with
// FPSCR[NI] off and on, and the result bits are hashed per helper. The program prints one line
// per helper; two hosts implement the same guest semantics exactly when their outputs are
// identical, so the check is `diff <(native) <(node wasm.js)`. The x86-64 build is the
// reference: its results are the ones verified against Dolphin and hardware.
//
// With --dump <helper> it instead prints every input/output for that helper, to locate a
// difference the hashes reported.

#include "isa/ppc_isa_quantized.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

struct Hasher {
    uint64_t value = 1469598103934665603ull;
    uint64_t count = 0;
    void Add(uint64_t bits) {
        value = (value ^ bits) * 1099511628211ull;
        ++count;
    }
};

const char* g_dumpName = nullptr;

uint64_t Bits(double value) { return PpcBitCastToU64Inline(value); }

// A float result carried through the double-typed harness without a host FP conversion.
double FloatBits(float value) {
    return PpcBitCastToDoubleInline(PpcBitCastToU32Inline(value));
}

// Edge cases as float bit patterns: signed zeros, denormal extremes, FLT_MIN and its neighbours,
// ordinary values, FLT_MAX, infinities, and quiet/signalling NaNs with distinct payloads.
const uint32_t kEdgeFloats[] = {
    0x00000000u, 0x80000000u, 0x00000001u, 0x80000001u, 0x00400000u, 0x007FFFFFu, 0x807FFFFFu,
    0x00800000u, 0x80800000u, 0x00800001u, 0x3F800000u, 0xBF800000u, 0x3F800001u, 0x3EAAAAABu,
    0x40490FDBu, 0x4B000001u, 0x7F7FFFFFu, 0xFF7FFFFFu, 0x7F800000u, 0xFF800000u, 0x7FC00000u,
    0xFFC00000u, 0x7FC12345u, 0xFFD54321u, 0x7F812345u, 0xFF854321u, 0x1A000000u, 0x26000000u,
};

// Edge cases as double bit patterns, including values that only become single denormals after
// narrowing and the FLT_MIN tie region.
const uint64_t kEdgeDoubles[] = {
    0x0000000000000000ull, 0x8000000000000000ull, 0x0000000000000001ull, 0x000FFFFFFFFFFFFFull,
    0x0010000000000000ull, 0x3810000000000000ull, 0xB810000000000000ull, 0x380FFFFFF0000000ull,
    0x380FFFFFE0000000ull, 0x36A0000000000000ull, 0x3690000000000000ull, 0x3FF0000000000000ull,
    0xBFF0000000000000ull, 0x3FF0000000000001ull, 0x3FD5555555555555ull, 0x400921FB54442D18ull,
    0x47EFFFFFE0000000ull, 0x47EFFFFFF0000000ull, 0x7FEFFFFFFFFFFFFFull, 0x7FF0000000000000ull,
    0xFFF0000000000000ull, 0x7FF8000000000000ull, 0xFFF8000000000000ull, 0x7FF8123456789ABCull,
    0x7FF0000000000001ull, 0xFFF4000000000001ull, 0x41DFFFFFFFC00000ull, 0xC1E0000000200000ull,
    // Around 2^-126 - 2^-151, where tininess detected before and after rounding disagree.
    0x380FFFFFF0000000ull, 0x380FFFFFF8000000ull, 0x380FFFFFE8000000ull, 0xB80FFFFFF8000000ull,
};

struct Rng {
    uint64_t state = 0x9E3779B97F4A7C15ull;
    uint64_t Next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }
};

// Random floats biased toward interesting exponents: around 1, around FLT_MIN (where NI acts),
// near overflow, and fully random bit patterns (which include NaNs and infinities).
uint32_t RandomFloatBits(Rng& rng) {
    const uint64_t r = rng.Next();
    const uint32_t sign = static_cast<uint32_t>(r >> 63) << 31;
    const uint32_t mantissa = static_cast<uint32_t>(r) & 0x007FFFFFu;
    switch ((r >> 40) & 7u) {
    case 0: case 1: case 2: return sign | ((static_cast<uint32_t>(120 + (r >> 32) % 16)) << 23) | mantissa;
    case 3: case 4: return sign | ((static_cast<uint32_t>((r >> 32) % 4)) << 23) | mantissa;
    case 5: return sign | ((static_cast<uint32_t>(250 + (r >> 32) % 5)) << 23) | mantissa;
    default: return static_cast<uint32_t>(r >> 16);
    }
}

uint64_t RandomDoubleBits(Rng& rng) {
    const uint64_t r = rng.Next();
    const uint64_t sign = r & 0x8000000000000000ull;
    const uint64_t mantissa = rng.Next() & 0x000FFFFFFFFFFFFFull;
    switch ((r >> 40) & 7u) {
    case 0: case 1: case 2: return sign | ((1015 + (r >> 20) % 16) << 52) | mantissa;
    case 3: case 4: return sign | ((870 + (r >> 20) % 30) << 52) | mantissa;  // single-denormal range
    case 5: return sign | ((1140 + (r >> 20) % 10) << 52) | mantissa;
    default: return rng.Next();
    }
}

struct Inputs {
    std::vector<double> paired;  // packed (ps0, ps1)
    std::vector<double> scalar;  // full doubles
};

Inputs MakeInputs() {
    Inputs inputs;
    for (uint32_t a : kEdgeFloats)
        for (uint32_t b : kEdgeFloats)
            inputs.paired.push_back(PpcPackPairedBitsInline(a, b));
    Rng rng;
    for (int i = 0; i < 1500; ++i)
        inputs.paired.push_back(PpcPackPairedBitsInline(RandomFloatBits(rng), RandomFloatBits(rng)));
    for (uint64_t bits : kEdgeDoubles)
        inputs.scalar.push_back(PpcBitCastToDoubleInline(bits));
    for (uint32_t bits : kEdgeFloats)
        inputs.scalar.push_back(static_cast<double>(PpcBitCastToFloatInline(bits)));
    for (int i = 0; i < 1500; ++i)
        inputs.scalar.push_back(PpcBitCastToDoubleInline(RandomDoubleBits(rng)));
    return inputs;
}

// Operand triples cover every combination for the edge block and a deterministic sample of the
// rest, which keeps the run in a few seconds even as WebAssembly.
template <typename Fn>
void ForTriples(const std::vector<double>& values, Fn fn) {
    const size_t n = values.size();
    Rng rng;
    for (size_t i = 0; i < 30 && i < n; ++i)
        for (size_t j = 0; j < 30 && j < n; ++j)
            for (size_t k = 0; k < 30 && k < n; ++k)
                fn(values[i], values[j], values[k]);
    for (int i = 0; i < 200000; ++i)
        fn(values[rng.Next() % n], values[rng.Next() % n], values[rng.Next() % n]);
}

template <typename Fn>
void ForPairs(const std::vector<double>& values, Fn fn) {
    const size_t n = values.size();
    Rng rng;
    for (size_t i = 0; i < 160 && i < n; ++i)
        for (size_t j = 0; j < 160 && j < n; ++j)
            fn(values[i], values[j]);
    for (int i = 0; i < 200000; ++i)
        fn(values[rng.Next() % n], values[rng.Next() % n]);
}

void Report(const char* name, bool ni, const Hasher& hash) {
    if (g_dumpName) return;
    std::printf("%-26s ni=%d n=%-8llu %016llx\n", name, ni ? 1 : 0,
                static_cast<unsigned long long>(hash.count),
                static_cast<unsigned long long>(hash.value));
}

bool Dumping(const char* name) { return g_dumpName && std::strcmp(g_dumpName, name) == 0; }

// With two or more NaN operands x86 itself has no single answer (the winning payload depends on
// the operand order clang picked at each call site, see the AArch64 note in ppc_isa_float.h), so
// such NaN results are hashed as "some NaN" rather than by payload. Single-NaN and invalid-op
// results keep their exact bits.
bool IsNanBits(uint32_t bits) { return (bits & 0x7FFFFFFFu) > 0x7F800000u; }
uint32_t Ps0(double v) { return static_cast<uint32_t>(Bits(v) >> 32); }
uint32_t Ps1(double v) { return static_cast<uint32_t>(Bits(v)); }

uint32_t CanonLane(uint32_t result, std::initializer_list<uint32_t> inputs) {
    int nans = 0;
    for (uint32_t bits : inputs) nans += IsNanBits(bits) ? 1 : 0;
    return (nans >= 2 && IsNanBits(result)) ? 0x7FFFFFFFu : result;
}

double CanonPair(double r, std::initializer_list<uint32_t> ps0, std::initializer_list<uint32_t> ps1) {
    return PpcPackPairedBitsInline(CanonLane(Ps0(r), ps0), CanonLane(Ps1(r), ps1));
}

double CanonScalar(double r, std::initializer_list<double> inputs) {
    int nans = 0;
    for (double v : inputs) nans += (v != v) ? 1 : 0;
    return (nans >= 2 && r != r) ? PpcBitCastToDoubleInline(0x7FFFFFFFFFFFFFFFull) : r;
}

#define LANES2 CanonPair(r, {Ps0(a), Ps0(b)}, {Ps1(a), Ps1(b)})
#define LANES3 CanonPair(r, {Ps0(a), Ps0(b), Ps0(c)}, {Ps1(a), Ps1(b), Ps1(c)})
#define SCALAR2 CanonScalar(r, {a, b})
#define SCALAR3 CanonScalar(r, {a, b, c})
#define EXACT r

#define UNARY(NAME, EXPR, INPUTS)                                                            \
    {                                                                                        \
        Hasher h;                                                                            \
        for (double a : inputs.INPUTS) {                                                     \
            const double r = (EXPR);                                                         \
            h.Add(Bits(r));                                                                  \
            if (Dumping(NAME))                                                               \
                std::printf("ni=%d %016llx -> %016llx\n", ni, (unsigned long long)Bits(a),  \
                            (unsigned long long)Bits(r));                                    \
        }                                                                                    \
        Report(NAME, ni, h);                                                                 \
    }

#define BINARY(NAME, EXPR, INPUTS, CANON)                                                           \
    {                                                                                        \
        Hasher h;                                                                            \
        ForPairs(inputs.INPUTS, [&](double a, double b) {                                    \
            double r = (EXPR);                                                               \
            r = (CANON);                                                                     \
            h.Add(Bits(r));                                                                  \
            if (Dumping(NAME))                                                               \
                std::printf("ni=%d %016llx %016llx -> %016llx\n", ni,                       \
                            (unsigned long long)Bits(a), (unsigned long long)Bits(b),        \
                            (unsigned long long)Bits(r));                                    \
        });                                                                                  \
        Report(NAME, ni, h);                                                                 \
    }

#define TERNARY(NAME, EXPR, INPUTS, CANON)                                                          \
    {                                                                                        \
        Hasher h;                                                                            \
        ForTriples(inputs.INPUTS, [&](double a, double b, double c) {                        \
            double r = (EXPR);                                                               \
            r = (CANON);                                                                     \
            h.Add(Bits(r));                                                                  \
            if (Dumping(NAME))                                                               \
                std::printf("ni=%d %016llx %016llx %016llx -> %016llx\n", ni,               \
                            (unsigned long long)Bits(a), (unsigned long long)Bits(b),        \
                            (unsigned long long)Bits(c), (unsigned long long)Bits(r));       \
        });                                                                                  \
        Report(NAME, ni, h);                                                                 \
    }

void RunAll(const Inputs& inputs, bool ni) {
    MkwApplyHostNiMode(ni ? 0x4u : 0x0u);

    // Paired singles.
    BINARY("ps_add", PPC_PsAddInline(a, b), paired, LANES2)
    BINARY("ps_sub", PPC_PsSubInline(a, b), paired, LANES2)
    BINARY("ps_mul", PPC_PsMulInline(a, b), paired, LANES2)
    BINARY("ps_div", PPC_PsDivInline(a, b), paired, LANES2)
    BINARY("ps_muls0", PPC_PsMuls0Inline(a, b), paired,
           CanonPair(r, {Ps0(a), Ps0(b)}, {Ps1(a), Ps0(b)}))
    BINARY("ps_muls1", PPC_PsMuls1Inline(a, b), paired,
           CanonPair(r, {Ps0(a), Ps1(b)}, {Ps1(a), Ps1(b)}))
    BINARY("ps_merge00", PPC_PsMerge00Inline(a, b), paired, EXACT)
    BINARY("ps_merge01", PPC_PsMerge01Inline(a, b), paired, EXACT)
    BINARY("ps_merge10", PPC_PsMerge10Inline(a, b), paired, EXACT)
    BINARY("ps_merge11", PPC_PsMerge11Inline(a, b), paired, EXACT)
    TERNARY("ps_madd", PPC_PsMaddInline(a, b, c), paired, LANES3)
    TERNARY("ps_msub", PPC_PsMsubInline(a, b, c), paired, LANES3)
    TERNARY("ps_nmadd", PPC_PsNmaddInline(a, b, c), paired, LANES3)
    TERNARY("ps_nmsub", PPC_PsNmsubInline(a, b, c), paired, LANES3)
    TERNARY("ps_madds0", PPC_PsMadds0Inline(a, b, c), paired,
            CanonPair(r, {Ps0(a), Ps0(b), Ps0(c)}, {Ps1(a), Ps0(b), Ps1(c)}))
    TERNARY("ps_madds1", PPC_PsMadds1Inline(a, b, c), paired,
            CanonPair(r, {Ps0(a), Ps1(b), Ps0(c)}, {Ps1(a), Ps1(b), Ps1(c)}))
    TERNARY("ps_sum0", PPC_PsSum0Inline(a, b, c), paired,
            CanonPair(r, {Ps0(a), Ps1(b)}, {Ps1(c)}))
    TERNARY("ps_sum1", PPC_PsSum1Inline(a, b, c), paired,
            CanonPair(r, {Ps0(c)}, {Ps0(a), Ps1(b)}))
    TERNARY("ps_sel", PPC_PsSelInline(a, b, c), paired, EXACT)
    UNARY("ps_neg", PPC_PsNegInline(a), paired)
    UNARY("ps_abs", PPC_PsAbsInline(a), paired)
    UNARY("ps_res", PPC_PsRes(a), paired)
    UNARY("ps_to_scalar", PPC_PsToScalarInline(a), paired)
    UNARY("ps_get_ps0", FloatBits(PpcGetPs0Inline(a)), paired)
    UNARY("ps_get_ps1", FloatBits(PpcGetPs1Inline(a)), paired)
    UNARY("ps_from_scalar", PPC_PsFromScalarInline(a), scalar)

    // Scalar double and single.
    TERNARY("fmadd", PpcFmaddInline(a, b, c), scalar, SCALAR3)
    TERNARY("fmsub", PpcFmsubInline(a, b, c), scalar, SCALAR3)
    TERNARY("fnmadd", PpcFnmaddInline(a, b, c), scalar, SCALAR3)
    TERNARY("fnmsub", PpcFnmsubInline(a, b, c), scalar, SCALAR3)
    TERNARY("fmadds", PpcFmaddsInline(a, b, c), scalar, SCALAR3)
    TERNARY("fmsubs", PpcFmsubsInline(a, b, c), scalar, SCALAR3)
    TERNARY("fnmadds", PpcFnmaddsInline(a, b, c), scalar, SCALAR3)
    TERNARY("fnmsubs", PpcFnmsubsInline(a, b, c), scalar, SCALAR3)
    TERNARY("fsel", PPC_Fsel(a, b, c), scalar, EXACT)
    BINARY("fmuls", PpcFmulsInline(a, b), scalar, SCALAR2)
    // NoNi forms are only emitted where the translator proved NI clear, so they are only
    // meaningful (and only compared) with NI off.
    if (!ni) BINARY("fmuls_noni", PpcFmulsNoNiInline(a, b), scalar, SCALAR2)
    UNARY("force_single", FloatBits(PpcForceSingleValueInline(a)), scalar)
    UNARY("force25", PpcForce25BitInline(a), scalar)
    UNARY("fres", PPC_Fres(a), scalar)
    UNARY("frsqrte", PPC_Frsqrte(a), scalar)
    UNARY("fctiwz", PPC_Fctiwz(a), scalar)

    MkwApplyHostNiMode(0x0u);
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--dump") == 0)
        g_dumpName = argv[2];
    if (!MkwHostFmaIsFused()) {
        std::printf("FATAL: host FMA is not fused; physics parity is impossible on this host\n");
        return 2;
    }
    const Inputs inputs = MakeInputs();
    RunAll(inputs, false);
    RunAll(inputs, true);
    return 0;
}
