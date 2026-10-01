#ifndef ONTOLOGY_FIXED_H__
#define ONTOLOGY_FIXED_H__

#include <cstdint>

// ---------------------------------------------------------------------------
// Fixed — a number on the causal path: Q32.32 in an int64.
//
// NOT A FAMILY, like Mat: vocabulary, something a value can have. What it is
// for is the rule the runtime runs on -- ANYTHING THAT CAN SET A TAG, GATE AN
// INTERACTION OR FEED A HASH IS COMPUTED THE SAME EVERYWHERE. A float cannot
// promise that across a native build and a WASM one: libm's transcendentals
// are not correctly rounded, a*b+c may or may not be fused, and one ulp of
// difference in a value that is hashed is a different history. An integer
// promises it by construction, so the rows of an OrderVector are these, and
// the float exists only where a picture is made from them (ToMatrix4).
//
// 32 BITS OF INTEGER, 32 OF FRACTION. Positions to two billion units at
// 2^-32 resolution, energies the same, fractions in [0,1] with 32 bits --
// enough that nothing here needs a second format, and a power of two so that
// scaling to and from the type is exact: a script's literal, parsed to a
// float, times 2^32, truncated, is one integer on every platform.
//
// MULTIPLY AND DIVIDE ARE EXACT IN 128 BITS: the product of two in-range
// values is whole before it is scaled back, and the quotient is the whole
// quotient, so there is no intermediate rounding that could depend on the
// machine. The DEFINITION is floor((a*b) / 2^32) and trunc((a*2^32) / b),
// taken in the low 64 bits -- an integer function with one answer -- and it
// has two spellings here: __int128 where the machine has it (one multiply
// and one divide instruction on x86-64), and 64-bit halves (fixed_detail)
// on WASM, which has no 128-bit instructions and where every __int128
// operation is a library call, the divide a 128-step loop -- thirty times
// the native cost on the causal path. Both spellings of one definition;
// OrderVectorTesterLoader holds them to the bit against each other.
//
// THE FUNCTIONS THAT ARE NOT ARITHMETIC -- sqrt, exp, sin, cos -- are written
// here from the arithmetic, with a fixed number of terms, so that their
// result is a function of their argument and nothing else. The causal path
// runs a few of them per interaction and the picture, which runs millions,
// uses the float ones on its own side of the boundary; a float appears in
// Sqrt only as a starting guess that the integer steps after it correct.
//
// NO IMPLICIT CONVERSION FROM FLOAT, on purpose: the boundary between the two
// worlds is a place worth being able to see in the code, so crossing it is a
// named call (From / ToFloat) and never an accident of overload resolution.
// ---------------------------------------------------------------------------
namespace fixed_detail
{
    // |a| * |b| as (hi, lo): four 32x32 products, which every target does
    // in one instruction each.
    inline void umul(uint64_t a, uint64_t b, uint64_t& hi, uint64_t& lo)
    {
        const uint64_t a0 = a & 0xffffffffull, a1 = a >> 32;
        const uint64_t b0 = b & 0xffffffffull, b1 = b >> 32;
        const uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
        const uint64_t mid = (p00 >> 32) + (p01 & 0xffffffffull) + (p10 & 0xffffffffull);
        lo = (mid << 32) | (p00 & 0xffffffffull);
        hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
    }

    inline int clz(uint64_t v)
    {
#if defined(__GNUC__) || defined(__clang__)
        return __builtin_clzll(v);
#else
        int n = 0; while (!(v & (1ull << 63))) { v <<= 1; ++n; } return n;
#endif
    }

    // (u1:u0) / v with u1 < v: the two-digit schoolbook division, base 2^32
    // (Hacker's Delight, divlu2). Native 64-bit operations throughout.
    inline uint64_t divlu(uint64_t u1, uint64_t u0, uint64_t v)
    {
        const uint64_t b = 1ull << 32;
        const int s = clz(v);
        v <<= s;
        const uint64_t vn1 = v >> 32, vn0 = v & 0xffffffffull;
        const uint64_t un32 = s ? (u1 << s) | (u0 >> (64 - s)) : u1;
        const uint64_t un10 = u0 << s;
        const uint64_t un1 = un10 >> 32, un0 = un10 & 0xffffffffull;
        uint64_t q1 = un32 / vn1, rhat = un32 - q1 * vn1;
        while (q1 >= b || q1 * vn0 > b * rhat + un1) { --q1; rhat += vn1; if (rhat >= b) break; }
        const uint64_t un21 = un32 * b + un1 - q1 * v;
        uint64_t q0 = un21 / vn1;
        rhat = un21 - q0 * vn1;
        while (q0 >= b || q0 * vn0 > b * rhat + un0) { --q0; rhat += vn1; if (rhat >= b) break; }
        return q1 * b + q0;
    }

    // The low 64 bits of the full quotient (hi:lo) / v, for any size of
    // quotient: the high digit first, its remainder leading the low one.
    inline uint64_t udiv128_low(uint64_t hi, uint64_t lo, uint64_t v)
    {
        const uint64_t r = hi % v;          // the high quotient digit itself is above 64 bits: dropped
        return divlu(r, lo, v);
    }
}

struct Fixed
{
    static constexpr int     SHIFT = 32;
    static constexpr int64_t ONE   = int64_t(1) << SHIFT;

    int64_t raw = 0;

    constexpr Fixed() = default;
    static constexpr Fixed FromRaw(int64_t r) { Fixed f; f.raw = r; return f; }
    static constexpr Fixed FromInt(int64_t i) { return FromRaw(i << SHIFT); }
    // The boundary in: exact scaling by a power of two, then truncation
    // toward zero. Deterministic for any finite float on any platform.
    static Fixed From(double d) { return FromRaw(static_cast<int64_t>(d * 4294967296.0)); }
    static Fixed From(float f)  { return From(static_cast<double>(f)); }

    // The boundary out. Where a picture is made.
    float  ToFloat()  const { return static_cast<float>(static_cast<double>(raw) / 4294967296.0); }
    double ToDouble() const { return static_cast<double>(raw) / 4294967296.0; }
    int64_t ToInt()   const { return raw >> SHIFT; }   // floor

    static constexpr Fixed Zero() { return FromRaw(0); }
    static constexpr Fixed One()  { return FromRaw(ONE); }
    static constexpr Fixed Half() { return FromRaw(ONE >> 1); }
    // The constants, as the integer nearest their Q32.32 value.
    static constexpr Fixed Pi()    { return FromRaw(13493037705LL); }   // 3.14159265358979 * 2^32
    static constexpr Fixed TwoPi() { return FromRaw(26986075410LL); }
    static constexpr Fixed Ln2()   { return FromRaw(2977044471LL); }    // 0.69314718055995 * 2^32

    // ── arithmetic ──────────────────────────────────────────────────────
    constexpr Fixed operator+(Fixed o) const { return FromRaw(raw + o.raw); }
    constexpr Fixed operator-(Fixed o) const { return FromRaw(raw - o.raw); }
    constexpr Fixed operator-()        const { return FromRaw(-raw); }
    // floor((a * b) / 2^32), low 64 bits, in halves: the magnitudes' product
    // shifted, rounded toward minus infinity when the sign is negative (what
    // an arithmetic shift of the signed product is).
    static int64_t MulHalves(int64_t a, int64_t b)
    {
        const bool neg = (a < 0) != (b < 0);
        uint64_t hi, lo;
        fixed_detail::umul(mag(a), mag(b), hi, lo);
        uint64_t q = (lo >> SHIFT) | (hi << (64 - SHIFT));
        if (neg)
        {
            if (lo & 0xffffffffull) ++q;   // ceil of the magnitude: floor of the negative
            q = 0 - q;
        }
        return static_cast<int64_t>(q);
    }
    // trunc((a * 2^32) / b), low 64 bits, in halves. b != 0.
    static int64_t DivHalves(int64_t a, int64_t b)
    {
        const bool neg = (a < 0) != (b < 0);
        const uint64_t n = mag(a);
        uint64_t q = fixed_detail::udiv128_low(n >> (64 - SHIFT), n << SHIFT, mag(b));
        if (neg) q = 0 - q;
        return static_cast<int64_t>(q);
    }

#if defined(__SIZEOF_INT128__) && !defined(__EMSCRIPTEN__)
    Fixed operator*(Fixed o) const
    {
        return FromRaw(static_cast<int64_t>((static_cast<__int128>(raw) * o.raw) >> SHIFT));
    }
    // Division by zero answers zero: a rate over no interval, a share of no
    // energy. Every caller here tests the denominator first; this is the
    // floor under a mistake, not a convention to lean on.
    Fixed operator/(Fixed o) const
    {
        if (o.raw == 0) return Zero();
        return FromRaw(static_cast<int64_t>((static_cast<__int128>(raw) << SHIFT) / o.raw));
    }
#else
    Fixed operator*(Fixed o) const { return FromRaw(MulHalves(raw, o.raw)); }
    Fixed operator/(Fixed o) const { return o.raw == 0 ? Zero() : FromRaw(DivHalves(raw, o.raw)); }
#endif
    Fixed& operator+=(Fixed o) { raw += o.raw; return *this; }
    Fixed& operator-=(Fixed o) { raw -= o.raw; return *this; }
    Fixed& operator*=(Fixed o) { *this = *this * o; return *this; }
    Fixed& operator/=(Fixed o) { *this = *this / o; return *this; }

    constexpr bool operator==(Fixed o) const { return raw == o.raw; }
    constexpr bool operator!=(Fixed o) const { return raw != o.raw; }
    constexpr bool operator< (Fixed o) const { return raw <  o.raw; }
    constexpr bool operator<=(Fixed o) const { return raw <= o.raw; }
    constexpr bool operator> (Fixed o) const { return raw >  o.raw; }
    constexpr bool operator>=(Fixed o) const { return raw >= o.raw; }

    // The magnitude, as a width that holds INT64_MIN's.
    static constexpr uint64_t mag(int64_t v) { return v < 0 ? 0 - static_cast<uint64_t>(v) : static_cast<uint64_t>(v); }

    constexpr bool IsZero()     const { return raw == 0; }
    constexpr bool IsPositive() const { return raw > 0; }
    constexpr Fixed Abs() const { return FromRaw(raw < 0 ? -raw : raw); }
    static constexpr Fixed Min(Fixed a, Fixed b) { return a.raw < b.raw ? a : b; }
    static constexpr Fixed Max(Fixed a, Fixed b) { return a.raw > b.raw ? a : b; }

    // ── the functions, from the arithmetic ──────────────────────────────

    /*
     * sqrt(x) = isqrt(x * 2^32) in Q32.32: the integer square root, exact --
     * the largest r with r*r <= n -- so the answer is a function of the
     * argument alone. A float gives the GUESS and nothing else: sqrt of the
     * raw value in double is within one unit of the true root (n < 2^95, so
     * the root is below 2^48 and a double's 53 bits hold it to the unit),
     * and the two loops after it settle on the exact floor whatever the
     * guess was, in a step or two. The same answer the bit-by-bit root
     * gave, at a fraction of its sixty-odd 128-bit iterations.
     */
    Fixed Sqrt() const
    {
        if (raw <= 0) return Zero();
        const uint64_t n_hi = static_cast<uint64_t>(raw) >> (64 - SHIFT);
        const uint64_t n_lo = static_cast<uint64_t>(raw) << SHIFT;
        // r*r > n, in halves: the root is below 2^48, so its square fits.
        auto above = [&](uint64_t r) { uint64_t hi, lo; fixed_detail::umul(r, r, hi, lo); return hi > n_hi || (hi == n_hi && lo > n_lo); };
        uint64_t r = static_cast<uint64_t>(__builtin_sqrt(static_cast<double>(raw)) * 65536.0);
        while (above(r)) --r;
        while (!above(r + 1)) ++r;
        return FromRaw(static_cast<int64_t>(r));
    }

    /*
     * exp(x): reduce by ln2 so the series runs on r in [0, ln2), then shift
     * by the integer part -- 2^k is exact in this representation. Sixteen
     * terms of the series carry r^16/16! below the last bit for r < 0.7.
     * Below 2^-32 the answer is zero and past 2^31 it saturates: both are
     * the honest edges of the range, not errors.
     */
    Fixed Exp() const
    {
        if (raw < -(int64_t(45) << SHIFT)) return Zero();
        if (raw >  (int64_t(21) << SHIFT)) return FromRaw(INT64_MAX);
        int64_t k = (*this / Ln2()).raw >> SHIFT;              // floor(x / ln2)
        Fixed   r = *this - FromInt(k) * Ln2();
        if (r.raw < 0) { r += Ln2(); --k; }
        Fixed sum = One(), term = One();
        for (int n = 1; n <= 16; ++n)
        {
            term = FromRaw((term * r).raw / n);     // == / FromInt(n), without the 128-bit divide
            sum += term;
        }
        if (k >= 0) return FromRaw(sum.raw << k);
        return FromRaw(sum.raw >> (-k));
    }

    // sin and cos by the series on the argument reduced to [-pi, pi]; the
    // twelve terms carry pi^25/25! below the last bit. One reduction, both
    // answers, because a rotation always wants the pair.
    void SinCos(Fixed& s, Fixed& c) const
    {
        Fixed x = *this;
        // k = round(x / 2pi); x -= k * 2pi
        const Fixed turns = x / TwoPi();
        const int64_t k = (turns.raw + (ONE >> 1)) >> SHIFT;
        x -= FromInt(k) * TwoPi();
        const Fixed x2 = x * x;
        Fixed sterm = x, ssum = x;       // x^1/1!
        Fixed cterm = One(), csum = One();
        for (int n = 1; n <= 12; ++n)
        {
            // sin: x^(2n+1)/(2n+1)!   cos: x^(2n)/(2n)!
            cterm = FromRaw(-(cterm * x2).raw / ((2 * n - 1) * (2 * n)));
            csum += cterm;
            sterm = FromRaw(-(sterm * x2).raw / ((2 * n) * (2 * n + 1)));
            ssum += sterm;
        }
        s = ssum; c = csum;
    }
    Fixed Sin() const { Fixed s, c; SinCos(s, c); return s; }
    Fixed Cos() const { Fixed s, c; SinCos(s, c); return c; }

    // The length of a three-vector, once, where every reader here wants it.
    static Fixed Length(Fixed x, Fixed y, Fixed z) { return (x * x + y * y + z * z).Sqrt(); }
};

// The rows of a causal record, mixed into one number: a splitmix finaliser
// over the raw integers, applied per word. A hash and not a checksum -- what
// it is for is to say that two histories are the same history, so a single
// bit of difference anywhere must move every bit of the answer.
inline uint64_t fixed_mix(uint64_t h, int64_t word)
{
    h ^= static_cast<uint64_t>(word) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    h ^= h >> 30; h *= 0xbf58476d1ce4e5b9ull;
    h ^= h >> 27; h *= 0x94d049bb133111ebull;
    h ^= h >> 31;
    return h;
}

#endif
