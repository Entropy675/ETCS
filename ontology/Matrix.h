#ifndef SUPERTYPE_MATRIX_H__
#define SUPERTYPE_MATRIX_H__


#include "../core_defs.h"
#include "Mat.h"
#include <cstdint>

// ---------------------------------------------------------------
// Matrix
// ---------------------------------------------------------------
//
// ONE STEP AND A FORWARD LINK in a transform chain. A Matrix is not
// a picture, a place, or an actor: it is the identity of one
// multiplication in a pipeline, and the pipeline is the ordered set
// of Matrix children an entity carries.
//
// A REFINEMENT OF Wrapper, and that is what makes the chain exist
// rather than needing to be built. MirrorBuffer already resolves a
// chain as "the owner's typed children tagged Wrapper, in ATTACH
// ORDER, filtered by Scope()" and already applies it in place on
// the payload inside writeRaw/readRaw. So attach order IS the
// chain order, a stage costs no allocation, and a type's stream
// functions apply the whole pipeline with no call site aware of it.
// Nothing here reimplements any of that; Matrix is a Wrapper whose
// transform happens to be a product.
//
// WRAP AND UNWRAP ARE NOT INVERSES, and that is the difference from
// every other wrapper. Framing and encryption are symmetric -- the
// far side must recover the payload the near side had, so Unwrap
// undoes Wrap by definition. A transform is UNEVEN: the two sides
// of the boundary are two different spaces, and arriving is not
// undoing. If you want to come back you chain the inverse, which is
// what inverses are for. So this family mandates no relationship
// between the two directions at all.
//
// WHICH SIDE MULTIPLIES, WITH WHAT, AND HOW IS THE CONCRETE'S. The
// matrix is the identity of the instance; the provider supplies the
// functions on it. A family that named the side would be describing
// one pipeline, the same reason Camera_ refuses to name a projection
// matrix and Drawable3D_ refuses to name a renderer.
//
// AND LEAVING THE SIDE OPEN IS WORTH SOMETHING BECAUSE OF Observable.
// A recipient-side multiply is LAZY: it happens only when a recipient
// actually takes an update, so a chain feeding a target nobody is
// reading costs nothing at all. A sender-side multiply is EAGER: one
// product serves every recipient, which is the right trade when there
// are many. Neither is correct in general, which is exactly why this
// family declines to pick.
//
// What makes the lazy form cheap is that the gate is already free.
// The Observable edge is a lock-free flag per observer -- a reader
// asks TakeObserved(its own RID) and gets an atomic load, so
// "should I transform at all" costs one load rather than a lock, a
// queue, or a comparison of what changed. A stage that multiplies on
// arrival and a reader that skips a clean edge compose into a
// pipeline that does work only where somebody is looking.
//
// THE VALUE LIVES HERE. This family OWNS its m-by-n elements rather
// than declaring an interface to them -- the same division Raster_
// makes for its buffer and InputSource_ for its event ring. A stage
// whose values were somewhere else would be a stage that could
// disagree with its own shape, and the shape is the only thing the
// chain checks.
//
// THE SHAPE IS THE CONSTRAINT: an n-by-m may feed an m-by-p and
// nothing else. Stated twice on purpose, at two different times --
// Mat<R,C>::operator* refuses a mismatched product at COMPILE time
// for concrete code, and ChainsInto below answers the same question
// at RESOLVE time for a chain that was assembled by attaching
// children. A dynamic chain cannot be checked statically, so the
// runtime form is not a weaker copy of the static one; it is the
// only form available where the chain is actually built.
//
// FIXED CAPACITY, runtime shape -- the same primitive arrangement
// TBuffer makes, and for the same reason: these cross a DSO
// boundary by value, and 4x4 is the case nearly every stage is.
// Overflow to a shared page is the same future Buffer's own comment
// reserves; a matrix that needs more than this today is a matrix
// that wants its own storage family.

class Matrix_ : virtual public ETCS::Entity
{
public:
    virtual ~Matrix_() = default;

    // 8x8. Big enough for every affine and projective stage in a 3D pipeline
    // with room to spare, small enough to pass by value without thinking.
    static constexpr uint32_t MAX_MATRIX_ELEMS = 64;

    uint32_t Rows() const { return m_rows; }
    uint32_t Cols() const { return m_cols; }
    uint32_t Count() const { return m_rows * m_cols; }
    bool     ShapeIs(uint32_t r, uint32_t c) const { return m_rows == r && m_cols == c; }

    // Row-major, like Mat. Out of range reads a scratch zero rather than
    // running off the end: a chain assembled at runtime can be wrong, and the
    // honest answer to "element (9,9) of a 4x4" is that there is not one.
    float&       At(uint32_t r, uint32_t c)       { return inRange(r, c) ? m_v[r * m_cols + c] : m_oob; }
    const float& At(uint32_t r, uint32_t c) const { return inRange(r, c) ? m_v[r * m_cols + c] : m_zero; }

    const float* Values() const { return m_v; }
    float*       Values()       { return m_v; }

    /*
     * THE CHAIN CONSTRAINT, asked of a neighbour rather than of a product.
     *
     * n-by-m feeds m-by-p: my columns must equal the next stage's rows. That
     * is the only thing that makes an ordered set of stages a pipeline rather
     * than a pile, and it is checkable the moment the order is known -- which
     * for a Wrapper chain is when the children are walked, not when the code
     * was written.
     */
    bool ChainsInto(const Matrix_& next) const
    {
        return m_cols != 0 && m_cols == next.m_rows;
    }

    // ── the typed bridge ─────────────────────────────────────────────────
    //
    // Concrete code works in Mat<R,C>, where the shape is static and a bad
    // product will not compile. These two are the only places the static and
    // the dynamic form meet, so they are the only places the shape has to be
    // asserted at all.
    template <uint32_t R, uint32_t C>
    void Load(const Mat<R, C>& m)
    {
        static_assert(R * C <= MAX_MATRIX_ELEMS, "matrix larger than a stage can hold");
        m_rows = R; m_cols = C;
        for (uint32_t i = 0; i < R * C; ++i) m_v[i] = m.v[i];
    }

    // Zero-shaped if this stage is not that shape -- the caller checks
    // ShapeIs, or accepts a zero product, which is what a mismatch means.
    template <uint32_t R, uint32_t C>
    Mat<R, C> As() const
    {
        Mat<R, C> out;
        if (!ShapeIs(R, C)) return out;
        for (uint32_t i = 0; i < R * C; ++i) out.v[i] = m_v[i];
        return out;
    }

protected:
    void SetShape(uint32_t r, uint32_t c)
    {
        if (r * c > MAX_MATRIX_ELEMS) { m_rows = m_cols = 0; return; }
        m_rows = r; m_cols = c;
    }

    uint32_t m_rows = 0;
    uint32_t m_cols = 0;
    float    m_v[MAX_MATRIX_ELEMS] = {};

private:
    bool inRange(uint32_t r, uint32_t c) const { return r < m_rows && c < m_cols; }
    float              m_oob  = 0.0f;   // written by an out-of-range At and ignored
    static const float m_zero;
};

inline const float Matrix_::m_zero = 0.0f;

#endif
