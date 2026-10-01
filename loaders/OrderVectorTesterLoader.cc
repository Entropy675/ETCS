// OrderVectorTesterLoader.cc
//
// THE CAUSAL ROWS ARE INTEGERS, AND THE OPERATIONS KEEP THEIR PROMISES.
//
// Fixed (ontology/Fixed.h) is Q32.32 with its own sqrt, exp, sin and cos;
// OrderVector (ontology/OrderVector.h) is four rows of it. What is checked:
//
//   1. Fixed's functions agree with the double ones to well past 1e-8 over
//      the ranges the rows use, and are exact where they can be (sqrt of a
//      square, exp(0), sin(0), 2^k).
//   2. The invariants the header states as structural actually hold through
//      the operation set: |O| <= 1 through impulses that fight the motion,
//      |K| never rises except through Impulse, Emit/Absorb move one number.
//   3. Row 3 composes as a rotation: quarter turns, and the matrix built from
//      it agrees with the vector it rotates.
//   4. Reduce and Cover: the aggregate encloses its members, GapTo is a proof.
//   5. DETERMINISM, the reason for all of it: the same sequence run twice
//      lands on the same rows bit for bit and the same Hash; the sequence's
//      hash is printed so the same script in a browser can be compared.
//
//   ./Run_OrderVectorTesterLoader
//
// No display, no runtime state beyond the arena.

#include "../ETCS.h"

#include <cmath>
#include <cstdio>

static int g_fail = 0;
static void check(bool ok, const char* what)
{
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}
static bool close(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

int main(int, char**)
{
    WIRE_CONTEXT();

    std::printf("-- Fixed: the arithmetic and the functions ------------------------\n");
    {
        const Fixed a = Fixed::From(1.5), b = Fixed::From(-0.25);
        check((a + b).ToDouble() == 1.25, "1.5 + -0.25 = 1.25, exactly");
        check((a * b).ToDouble() == -0.375, "1.5 * -0.25 = -0.375, exactly");
        check((a / b).ToDouble() == -6.0, "1.5 / -0.25 = -6, exactly");
        check(Fixed::FromInt(144).Sqrt() == Fixed::FromInt(12), "sqrt(144) = 12, exactly");
        check(Fixed::From(2.25).Sqrt() == Fixed::From(1.5), "sqrt(2.25) = 1.5, exactly");
        check(close(Fixed::FromInt(2).Sqrt().ToDouble(), std::sqrt(2.0), 1e-9), "sqrt(2) to 1e-9");
        check(Fixed::Zero().Exp() == Fixed::One(), "exp(0) = 1, exactly");
        check(Fixed::Ln2().Exp().raw == Fixed::FromInt(2).raw || close(Fixed::Ln2().Exp().ToDouble(), 2.0, 1e-9), "exp(ln2) = 2 to 1e-9");
        bool exp_ok = true;
        for (double x = -20.0; x <= 5.0; x += 0.37)
            if (!close(Fixed::From(x).Exp().ToDouble(), std::exp(x), std::exp(x) * 1e-8 + 1e-9)) exp_ok = false;
        check(exp_ok, "exp over [-20, 5] to 1e-8 relative");
        bool trig_ok = true;
        for (double x = -20.0; x <= 20.0; x += 0.173)
        {
            Fixed s, c; Fixed::From(x).SinCos(s, c);
            if (!close(s.ToDouble(), std::sin(x), 1e-8) || !close(c.ToDouble(), std::cos(x), 1e-8)) trig_ok = false;
        }
        check(trig_ok, "sin and cos over [-20, 20] to 1e-8");
        check(Fixed::Zero().Sin() == Fixed::Zero() && Fixed::Zero().Cos() == Fixed::One(), "sin(0) = 0, cos(0) = 1, exactly");
        check(Fixed::From(3.0).Exp().ToDouble() < 20.1 && Fixed::From(3.0).Exp().ToDouble() > 20.0, "exp(3) ~ 20.09");
        check(Fixed::FromInt(-100).Exp().IsZero(), "exp(-100) underflows to zero, honestly");

        /*
         * THE HALVES AGAINST THE WHOLE. Fixed has two spellings of one
         * definition -- floor((a*b)/2^32) and trunc((a*2^32)/b) in the low
         * 64 bits -- __int128 natively and 64-bit halves on WASM
         * (Fixed::MulHalves/DivHalves, fixed_detail). This tester runs where
         * __int128 exists, so it holds the halves to the reference bit for
         * bit over a splitmix walk of the whole range: small, huge, mixed
         * signs, the overflowing quotients included. What the browser runs
         * is thereby what the native build runs.
         */
        {
            uint64_t st = 0x9e3779b97f4a7c15ull;
            auto next = [&st]() { st += 0x9e3779b97f4a7c15ull; uint64_t z = st; z ^= z >> 30; z *= 0xbf58476d1ce4e5b9ull; z ^= z >> 27; z *= 0x94d049bb133111ebull; return z ^ (z >> 31); };
            auto draw = [&next]() -> int64_t {
                const uint64_t r = next();
                const int width = static_cast<int>(r % 64) + 1;          // every magnitude class
                const uint64_t m = width == 64 ? next() : (next() & ((1ull << width) - 1));
                return (r & 1) ? -static_cast<int64_t>(m) : static_cast<int64_t>(m);
            };
            bool mul_ok = true, div_ok = true, sqrt_ok = true;
            for (int i = 0; i < 200000; ++i)
            {
                const int64_t a = draw(), b = draw();
                const int64_t m_ref = static_cast<int64_t>((static_cast<__int128>(a) * b) >> 32);
                if (Fixed::MulHalves(a, b) != m_ref || (Fixed::FromRaw(a) * Fixed::FromRaw(b)).raw != m_ref) mul_ok = false;
                if (b != 0)
                {
                    const int64_t d_ref = static_cast<int64_t>((static_cast<__int128>(a) << 32) / b);
                    if (Fixed::DivHalves(a, b) != d_ref || (Fixed::FromRaw(a) / Fixed::FromRaw(b)).raw != d_ref) div_ok = false;
                }
                if (a > 0)
                {
                    const unsigned __int128 n = static_cast<unsigned __int128>(a) << 32;
                    unsigned __int128 r = Fixed::FromRaw(a).Sqrt().raw;
                    if (r * r > n || (r + 1) * (r + 1) <= n) sqrt_ok = false;
                }
            }
            check(mul_ok,  "200,000 products agree with __int128 to the bit, both signs, every width");
            check(div_ok,  "200,000 quotients agree with __int128 to the bit, overflowing ones included");
            check(sqrt_ok, "200,000 roots are the exact floor root");
        }
    }

    std::printf("\n-- the invariants ---------------------------------------------------\n");
    {
        OrderVector p; p.rid = 7;
        p.Impulse(Fixed::One(), Fixed::Zero(), Fixed::Zero(), Fixed::FromInt(10));
        check(p.energy == Fixed::FromInt(10), "an impulse of 10 J puts 10 J on the point");
        check(close(p.KineticFraction().ToDouble(), 1.0, 1e-9), "all of it kinetic: |O| = 1");
        p.Impulse(-Fixed::One(), Fixed::Zero(), Fixed::Zero(), Fixed::FromInt(10));
        check(p.energy == Fixed::FromInt(20), "pushing back costs the same 10 J");
        check(p.KineticEnergy().raw <= Fixed::From(1e-6).raw, "and cancels the motion: |K| ~ 0, the rest is heat");
        check(close(p.Heat().ToDouble(), 20.0, 1e-6), "heat = E - |K| = 20");
        p.Impulse(Fixed::Zero(), Fixed::One(), Fixed::Zero(), Fixed::FromInt(5));
        const Fixed k_before = p.KineticEnergy();
        p.Dissipate(Fixed::Half());
        check(p.energy == Fixed::FromInt(25) && p.KineticEnergy() < k_before, "Dissipate halves |K| and leaves E");
        check(close(p.KineticEnergy().ToDouble(), k_before.ToDouble() * 0.5, 1e-6), "...by exactly the factor");
        // |O| <= 1 through a storm of impulses in every direction
        bool bounded = true;
        OrderVector q; q.rid = 8;
        for (int i = 0; i < 200; ++i)
        {
            q.Impulse(Fixed::From(std::sin(i * 0.7)), Fixed::From(std::cos(i * 1.3)), Fixed::From(std::sin(i * 0.2)), Fixed::From(0.5 + (i % 7)));
            if (q.KineticFraction() > Fixed::One() + Fixed::From(1e-9)) bounded = false;
            if (i % 3 == 0) q.Dissipate(Fixed::From(0.9));
        }
        check(bounded, "|O| <= 1 through 200 impulses in every direction");
        // Emit / Absorb: one number moves
        OrderVector env; env.rid = 1; env.energy = Fixed::FromInt(100);
        const Fixed e_total = p.energy + env.energy;
        const Fixed k_held = p.KineticEnergy();
        const OrderVector crossing = p.EmitEvent(Fixed::FromInt(4), Fixed::From(0.5));
        env.Absorb(crossing);
        check(crossing.energy == Fixed::FromInt(4), "EmitEvent hands out exactly 4 J of heat");
        check(p.energy + env.energy == e_total, "...and the total is conserved to the bit");
        check(close(p.KineticEnergy().ToDouble(), k_held.ToDouble(), 1e-6), "|K| is held through the emission: |O| rose");
        check(crossing.uncertainty != 0 && crossing.uncertainty == OrderVector::derive_uncertainty(crossing), "the crossing's uncertainty is a function of the crossing");
        OrderVector cold; cold.rid = 9; cold.energy = Fixed::FromInt(3);
        cold.Impulse(Fixed::One(), Fixed::Zero(), Fixed::Zero(), Fixed::FromInt(3));   // E=6, K=3
        const Fixed took = cold.Emit(Fixed::FromInt(100));
        check(close(took.ToDouble(), 3.0, 1e-6), "Emit is capped at the heat there is");
        check(cold.EmissionOver(Fixed::One(), Fixed::One()).IsZero(), "a point with no heat emits nothing");
        // EmissionOver is lazy-exact: once over T equals twice over T/2
        OrderVector w1; w1.energy = Fixed::FromInt(50);
        OrderVector w2 = w1;
        const Fixed k = Fixed::From(0.3);
        w1.Emit(w1.EmissionOver(Fixed::One(), k));
        w2.Emit(w2.EmissionOver(Fixed::Half(), k)); w2.Emit(w2.EmissionOver(Fixed::Half(), k));
        check(close(w1.energy.ToDouble(), w2.energy.ToDouble(), 1e-7), "one commit over T equals two over T/2 (lazy commit is exact)");
    }

    std::printf("\n-- row 3: the spinor ------------------------------------------------\n");
    {
        OrderVector r;
        check(!r.Oriented(), "a fresh vector is unrotated");
        r.Orient(Fixed::Zero(), Fixed::One(), Fixed::Zero(), Fixed::Pi() * Fixed::Half());   // 90 deg about y
        Fixed vx = Fixed::One(), vy = Fixed::Zero(), vz = Fixed::Zero();
        r.RotateVector(vx, vy, vz);
        check(close(vx.ToDouble(), 0.0, 1e-8) && close(vz.ToDouble(), -1.0, 1e-8), "a quarter turn about y takes +x to -z");
        r.RotateBy(Fixed::Zero(), Fixed::One(), Fixed::Zero(), Fixed::Pi() * Fixed::Half());
        vx = Fixed::One(); vy = Fixed::Zero(); vz = Fixed::Zero();
        r.RotateVector(vx, vy, vz);
        check(close(vx.ToDouble(), -1.0, 1e-7), "two quarter turns take +x to -x");
        const Fixed n = (r.qw * r.qw + r.qx * r.qx + r.qy * r.qy + r.qz * r.qz).Sqrt();
        check(close(n.ToDouble(), 1.0, 1e-9), "the spinor stays unit through composition");
        const Matrix4 m = r.ToMatrix4();
        check(close(m.at(0,0), -1.0, 1e-6) && close(m.at(2,2), -1.0, 1e-6), "and the matrix built from it is the half turn");
        r.PlaceAt(Fixed::FromInt(3), Fixed::FromInt(4), Fixed::FromInt(5));
        const Matrix4 t = r.ToMatrix4();
        check(t.at(0,3) == 3.0f && t.at(1,3) == 4.0f && t.at(2,3) == 5.0f && t.at(3,3) == 1.0f, "position is the translation column; the RID is nowhere in it");
        r.SetPivot(Fixed::One(), Fixed::Zero(), Fixed::Zero());
        Fixed px = Fixed::FromInt(2), py = Fixed::Zero(), pz = Fixed::Zero();
        r.RotateAboutPivot(px, py, pz);
        check(close(px.ToDouble(), 0.0, 1e-7), "about a pivot at x=1, the point at x=2 lands at x=0 after a half turn");
    }

    std::printf("\n-- aggregation ------------------------------------------------------\n");
    {
        OrderVector m[3];
        for (int i = 0; i < 3; ++i) { m[i].rid = 10 + i; m[i].energy = Fixed::FromInt(1); }
        m[0].PlaceAt(Fixed::FromInt(-2), Fixed::Zero(), Fixed::Zero());
        m[2].PlaceAt(Fixed::FromInt(2), Fixed::Zero(), Fixed::Zero());
        m[2].radius = Fixed::One();
        OrderVector agg; agg.rid = 99;
        agg.Reduce(m, 3);
        check(agg.IsAggregate() && agg.x.IsZero(), "the reduction is an aggregate at the energy-weighted centre");
        check(agg.radius == Fixed::FromInt(3), "its reach covers the farthest member plus that member's own radius: 2 + 1");
        check(agg.energy == Fixed::FromInt(3), "energy adds");
        check(agg.Encloses(Fixed::From(2.5), Fixed::Zero(), Fixed::Zero()) && !agg.Encloses(Fixed::From(3.5), Fixed::Zero(), Fixed::Zero()), "Encloses answers off the radius");
        OrderVector far; far.PlaceAt(Fixed::FromInt(10), Fixed::Zero(), Fixed::Zero()); far.radius = Fixed::One();
        check(agg.GapTo(far) == Fixed::FromInt(6) && !agg.MayInteractWith(far), "a gap of 6 is a proof: nothing under either can have met");
        far.PlaceAt(Fixed::FromInt(4), Fixed::Zero(), Fixed::Zero());
        check(agg.MayInteractWith(far), "touching reaches refer the question to the members");
        check(!m[0].Encloses(m[0].x, m[0].y, m[0].z), "a leaf encloses nothing, itself included");
    }

    std::printf("\n-- determinism ------------------------------------------------------\n");
    {
        auto run = [](OrderVector& p) {
            p.rid = 42;
            for (int i = 0; i < 500; ++i)
            {
                p.Impulse(Fixed::From(std::sin(i * 0.31)), Fixed::From(0.2), Fixed::From(std::cos(i * 0.17)), Fixed::From(1.0 + (i % 5)));
                p.Dissipate((-(Fixed::From(0.8) * Fixed::From(0.016))).Exp());
                p.Advance(Fixed::From(0.016), Fixed::FromInt(2));
                if (i % 7 == 0) p.EmitEvent(p.EmissionOver(Fixed::From(0.1), Fixed::Half()), Fixed::From(0.1));
                if (i % 11 == 0) p.RotateBy(Fixed::From(0.3), Fixed::One(), Fixed::From(-0.2), Fixed::From(0.05));
            }
        };
        OrderVector a, b;
        run(a); run(b);
        check(a.Hash() == b.Hash(), "the same 500 steps twice: the same hash");
        check(a.x == b.x && a.energy == b.energy && a.qw == b.qw, "...and the same rows, bit for bit");
        b.x = Fixed::FromRaw(b.x.raw + 1);
        check(a.Hash() != b.Hash(), "one bit in one row is a different hash");
        std::printf("  sequence hash: %016llx   (compare against the same steps on another platform)\n",
                    static_cast<unsigned long long>(a.Hash()));
    }

    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
