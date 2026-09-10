#ifndef ONTOLOGY_MAT_H__
#define ONTOLOGY_MAT_H__

#include <cstdint>
#include <cmath>

// ---------------------------------------------------------------------------
// Mat<R, C> — a matrix, with its shape in its type.
//
// NOT A FAMILY, the same way Point2D/Rect2D/Point3D are not: this is
// vocabulary. A family is something an entity can BE; this is something a
// value can have. Matrix (ontology/Matrix.h) is the family, and it is a
// participant in a transform chain -- a different claim entirely from "here
// are R*C floats and how to multiply them".
//
// THE SHAPE IS IN THE TYPE, WHICH IS WHERE THE CONSTRAINT LIVES. An n-by-m
// may multiply an m-by-p and nothing else, and the interesting thing about
// that rule is that it needs no runtime check and no assertion: operator*
// below takes a Mat<C, P>, so the shared inner dimension is the only thing it
// will bind to. A mismatched product is a type error at the call site, with
// the two shapes named in the message.
//
// ROW-MAJOR, stated because a matrix library that does not say is a matrix
// library you have to read. at(r, c) is the only accessor, so nothing outside
// this file depends on the choice.
//
// WHY A TEMPLATE RATHER THAN A 4x4. Because the chain is the point (see
// Matrix.h): a stage that takes n rows and yields m is a step in a pipeline
// whose ends have to agree, and a fixed 4x4 could express neither a
// projection that drops a dimension nor a chain that widens one. The 4x4 is
// the common case, named below, not the only one.
// ---------------------------------------------------------------------------
template <uint32_t R, uint32_t C>
struct Mat
{
    static constexpr uint32_t rows = R;
    static constexpr uint32_t cols = C;

    float v[R * C] = {};

    float&       at(uint32_t r, uint32_t c)       { return v[r * C + c]; }
    const float& at(uint32_t r, uint32_t c) const { return v[r * C + c]; }

    // Square only, and the compiler enforces it rather than a comment: a
    // non-square identity is not a thing that exists.
    static Mat Identity()
    {
        static_assert(R == C, "Identity is only defined for a square matrix");
        Mat m;
        for (uint32_t i = 0; i < R; ++i) m.at(i, i) = 1.0f;
        return m;
    }

    /*
     * n-by-m times m-by-p yields n-by-p, and THE SHARED DIMENSION IS THE
     * SIGNATURE. The right operand is a Mat<C, P>: C is this matrix's own
     * column count, so the only matrices that bind here are the ones whose
     * row count already agrees. That is the whole of the constraint, and it
     * costs nothing at runtime because it was never a runtime question.
     */
    template <uint32_t P>
    Mat<R, P> operator*(const Mat<C, P>& rhs) const
    {
        Mat<R, P> out;
        for (uint32_t r = 0; r < R; ++r)
            for (uint32_t k = 0; k < C; ++k)
            {
                const float a = at(r, k);
                if (a == 0.0f) continue;
                for (uint32_t c = 0; c < P; ++c) out.at(r, c) += a * rhs.at(k, c);
            }
        return out;
    }

    bool operator==(const Mat& o) const
    {
        for (uint32_t i = 0; i < R * C; ++i) if (v[i] != o.v[i]) return false;
        return true;
    }
};

// The common case, named because most of the pipeline is affine 3D and
// spelling Mat<4,4> at every site would say less than this does.
using Matrix4 = Mat<4, 4>;

#endif
