#ifndef ONTOLOGY_NOISE_H__
#define ONTOLOGY_NOISE_H__

#include "Fixed.h"
#include <cmath>
#include <cstdint>

// ---------------------------------------------------------------
// Noise
// ---------------------------------------------------------------
//
// NOT A FAMILY, like Fixed and Mat: vocabulary. A noise is a pure function of
// a point and a seed -- no state, no identity, nothing a record could say --
// so it is a TYPE, and what uses one takes it in a type slot (Warp<N, T>
// below), chosen where it is compiled, rather than an entity found by RID at
// run time. It is in the ontology, and so in the epoch, because a noise can
// decide a state: a kart's fire is cut and does damage where its warp puts
// it, so two runtimes that would warp differently must not interact.
//
// A NOISE TYPE is a struct with two `At`s, one in each number system: one in
// the rows' integers (Fixed), the same on every runtime, for what a state
// decides with; one in floats, for a picture redrawing that state, off by no
// more than a float's rounding. The two hash alike, so a seed is the same
// field in both. Perlin is the first; another (simplex, Worley) is another
// struct of the same shape beside it, and a slot takes either.
namespace ETCS { namespace Noise {

namespace detail {
inline uint32_t Hash(int32_t x, int32_t y, int32_t z, uint32_t seed)
{
    uint32_t h = seed * 0x9E3779B1u;
    h ^= static_cast<uint32_t>(x) * 0x85EBCA77u; h = (h << 13) | (h >> 19);
    h ^= static_cast<uint32_t>(y) * 0xC2B2AE3Du; h = (h << 13) | (h >> 19);
    h ^= static_cast<uint32_t>(z) * 0x27D4EB2Fu;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return h;
}
// The cube's twelve edge directions.
inline void Gradient(uint32_t h, int& gx, int& gy, int& gz)
{
    static const int g[12][3] = { { 1, 1, 0 }, { -1, 1, 0 }, { 1, -1, 0 }, { -1, -1, 0 },
                                  { 1, 0, 1 }, { -1, 0, 1 }, { 1, 0, -1 }, { -1, 0, -1 },
                                  { 0, 1, 1 }, { 0, -1, 1 }, { 0, 1, -1 }, { 0, -1, -1 } };
    const int* v = g[h % 12];
    gx = v[0]; gy = v[1]; gz = v[2];
}
inline Fixed Length(Fixed x, Fixed y, Fixed z) { return Fixed::Length(x, y, z); }
inline float Length(float x, float y, float z) { return std::sqrt(x * x + y * y + z * z); }
} // namespace detail

/*
 * PERLIN'S GRADIENT NOISE. Space is cut into unit cubes; every corner gets a
 * gradient -- one of the cube's twelve edge directions, picked by hashing the
 * corner's integers with the seed -- and the noise at a point is each
 * corner's gradient dotted with the way to the point, blended over the cube
 * by the quintic fade (6t^5 - 15t^4 + 10t^3), so the field and its slope are
 * continuous: smooth in all three axes, about -1..1, zero at every corner.
 */
struct Perlin
{
    static Fixed At(Fixed x, Fixed y, Fixed z, uint32_t seed)
    {
        const int32_t ix = static_cast<int32_t>(x.raw >> Fixed::SHIFT), iy = static_cast<int32_t>(y.raw >> Fixed::SHIFT),
                      iz = static_cast<int32_t>(z.raw >> Fixed::SHIFT);
        const Fixed fx = Fixed::FromRaw(x.raw & (Fixed::ONE - 1)), fy = Fixed::FromRaw(y.raw & (Fixed::ONE - 1)),
                    fz = Fixed::FromRaw(z.raw & (Fixed::ONE - 1));
        auto fade = [](Fixed t) { return t * t * t * (t * (t * Fixed::FromInt(6) - Fixed::FromInt(15)) + Fixed::FromInt(10)); };
        auto corner = [&](int dx, int dy, int dz) {
            int gx, gy, gz;
            detail::Gradient(detail::Hash(ix + dx, iy + dy, iz + dz, seed), gx, gy, gz);
            const Fixed ox = fx - Fixed::FromInt(dx), oy = fy - Fixed::FromInt(dy), oz = fz - Fixed::FromInt(dz);
            return Fixed::FromRaw(gx * ox.raw + gy * oy.raw + gz * oz.raw);
        };
        auto lerp = [](Fixed a, Fixed b, Fixed t) { return a + (b - a) * t; };
        const Fixed u = fade(fx), v = fade(fy), w = fade(fz);
        return lerp(lerp(lerp(corner(0, 0, 0), corner(1, 0, 0), u), lerp(corner(0, 1, 0), corner(1, 1, 0), u), v),
                    lerp(lerp(corner(0, 0, 1), corner(1, 0, 1), u), lerp(corner(0, 1, 1), corner(1, 1, 1), u), v), w);
    }
    static float At(float x, float y, float z, uint32_t seed)
    {
        const float flx = std::floor(x), fly = std::floor(y), flz = std::floor(z);
        const int32_t ix = static_cast<int32_t>(flx), iy = static_cast<int32_t>(fly), iz = static_cast<int32_t>(flz);
        const float fx = x - flx, fy = y - fly, fz = z - flz;
        auto fade = [](float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); };
        auto corner = [&](int dx, int dy, int dz) {
            int gx, gy, gz;
            detail::Gradient(detail::Hash(ix + dx, iy + dy, iz + dz, seed), gx, gy, gz);
            return gx * (fx - dx) + gy * (fy - dy) + gz * (fz - dz);
        };
        auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
        const float u = fade(fx), v = fade(fy), w = fade(fz);
        return lerp(lerp(lerp(corner(0, 0, 0), corner(1, 0, 0), u), lerp(corner(0, 1, 0), corner(1, 1, 0), u), v),
                    lerp(lerp(corner(0, 0, 1), corner(1, 0, 1), u), lerp(corner(0, 1, 1), corner(1, 1, 1), u), v), w);
    }
};

/*
 * A WARP: a noise N in each of the three axes (three seeds), `freq` cells to
 * a unit, its push at a point `amp` times the point's distance from (0, 0,
 * 0) -- nothing at the middle, most at the ends, so all of a shape curves
 * along its length rather than one part of it kinking. T is the number
 * system: Fixed where a state decides, float where a picture redraws it;
 * the one formula is both, so what is drawn is what was decided.
 */
template <typename N, typename T>
struct Warp
{
    uint32_t seed = 0;
    T        freq{}, amp{};

    // The push at (x, y, z), added to (px, py, pz).
    void Push(T x, T y, T z, T& px, T& py, T& pz) const
    {
        const T far = detail::Length(x, y, z) * amp, wx = x * freq, wy = y * freq, wz = z * freq;
        px += far * N::At(wx, wy, wz, seed);
        py += far * N::At(wx, wy, wz, seed + 0x51u);
        pz += far * N::At(wx, wy, wz, seed + 0xA3u);
    }
};

}} // namespace ETCS::Noise

#endif // ONTOLOGY_NOISE_H__
