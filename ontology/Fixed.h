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
// MULTIPLY AND DIVIDE GO THROUGH 128 BITS, which every target this runs on
// has (a compiler-lowered pair of words on WASM), so a product of two
// in-range values is exact before it is scaled back, and there is no
// intermediate rounding that could depend on the machine.
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
        const unsigned __int128 n = static_cast<unsigned __int128>(raw) << SHIFT;
        unsigned __int128 r = static_cast<unsigned __int128>(__builtin_sqrt(static_cast<double>(raw)) * 65536.0);
        while (r * r > n) --r;
        while ((r + 1) * (r + 1) <= n) ++r;
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
