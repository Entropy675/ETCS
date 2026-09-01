#ifndef BASE_ORDERABLE_H__
#define BASE_ORDERABLE_H__
#include "Orderable.h"
#include <type_traits>
#include <utility>

// Detection for "the leaf declares operator<". Deliberately spelled on a
// const lvalue: a comparison that needs a mutable operand is not a
// comparison, and catching that here beats catching it inside a sort.
namespace ETCS { namespace detail {
template <typename T, typename = void>
struct has_less : std::false_type {};
template <typename T>
struct has_less<T, std::void_t<decltype(std::declval<const T&>() < std::declval<const T&>())>>
    : std::true_type {};
} }

// Requires operator< on the leaf, derives the other five from it, and
// registers the "Orderable" interface family. See Orderable.h for what the
// family means and why the relation lives on the pointee rather than on the
// pointer.
//
// HOW THE REQUIREMENT IS CHECKED. Derived is incomplete at the point this
// template is instantiated -- that is what CRTP is -- so a static_assert in
// the class body would fire against a type that has not declared its members
// yet, and always fail. It lives in a VIRTUAL member's body instead: virtual
// function bodies are instantiated when the vtable is emitted, by which time
// the leaf is complete. Confirmed both ways -- silent for a leaf that
// declares operator<, and a named error for one that does not, at the point
// the leaf is actually instantiated rather than at some unrelated call site.
//
// The derived operators take `const Derived&` rather than `const
// OrderableBase&` so they compare leaves, not base subobjects, and so a
// heterogeneous comparison is a compile error rather than a silent slice.
// Two different concrete types have no relation under this family; ordering
// a mixed set is a different question with a different answer (see
// Drawable.h's own note on the merge).
ETCS_SUPERTYPE_BASE(Orderable)
{
    ETCS_MAKE_INSTANCE(Orderable)

    // The requirement. Never called; it exists to be instantiated.
    virtual void _etcs_orderable_requires_operator_less()
    {
        static_assert(ETCS::detail::has_less<Derived>::value,
            "Orderable requires the concrete type to declare "
            "bool operator<(const T&) const. Every other comparison is "
            "derived from it -- declare that one and only that one.");
    }

    // The other five, all in terms of the one. A type cannot make these
    // disagree with each other, because there is nothing here for it to
    // override -- the only knob is operator< itself.
    //
    // == is EQUIVALENCE under the ordering (neither precedes the other),
    // not identity. Entity identity is the RID. See Orderable.h.
    bool operator>(const Derived& o) const  { return o < self(); }
    bool operator<=(const Derived& o) const { return !(o < self()); }
    bool operator>=(const Derived& o) const { return !(self() < o); }
    bool operator==(const Derived& o) const { return !(self() < o) && !(o < self()); }
    bool operator!=(const Derived& o) const { return  (self() < o) ||  (o < self()); }

private:
    const Derived& self() const { return static_cast<const Derived&>(*this); }
};

#endif
