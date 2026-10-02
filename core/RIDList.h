#ifndef RIDLIST_H__
#define RIDLIST_H__
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <type_traits>
#include <utility>
#include "Buffer.h"
#include "ArenaAllocator.h"
namespace ETCS {
using RID = ETCS_RID_SIZE;
class Entity;
// Entity is incomplete here and stays that way (it includes this header),
// so the handle lambdas below cannot call a member on it: the expression is
// non-dependent, and lookup happens where the template is DEFINED. This is
// declared here and defined in Entity.h, where the class is complete --
// same pointer getTrueType() returns, one indirection later.
void* etcs_true_type(Entity* e);

// ── ordering detection ────────────────────────────────────────────────────────
//
// "Does the POINTEE declare operator<", never "does T". T here is always a
// pointer (the static_assert on RIDList below says so), and a comparison
// between two pointers is the built-in one: you cannot overload operator<
// for two pointers at all -- at least one operand must be a class or enum
// type -- so `a < b` on two T would silently compare ADDRESSES. That
// compiles, produces a deterministic order, and looks exactly like it
// worked; you would only catch it by noticing the order happened to match
// allocation order. (It is also formally unspecified for unrelated
// pointers; only ::std::less over pointers is a guaranteed total order.)
//
// So the relation is read one level in, off the pointee, where a type CAN
// declare it. A list whose pointee declares nothing keeps today's behaviour
// exactly and instantiates none of the machinery below -- the ordered view,
// its storage and its rebuild all live behind `if constexpr`.
namespace detail {
template <typename P, typename = void>
struct has_ordered_pointee : ::std::false_type {};
template <typename P>
struct has_ordered_pointee<P, ::std::void_t<decltype(
        ::std::declval<const ::std::remove_pointer_t<P>&>()
      < ::std::declval<const ::std::remove_pointer_t<P>&>())>>
    : ::std::true_type {};
template <typename P>
inline constexpr bool has_ordered_pointee_v = has_ordered_pointee<P>::value;

/*
 * A STABLE MERGE SORT WITH EVERY INDEX CHECKED. std::stable_sort's insertion
 * pass walks left without a bound once it has asked the relation once, trusting
 * it to answer the same way again; a key that commits mid-sort breaks that
 * trust and the walk leaves the vector. Here a relation that contradicts itself
 * yields SOME permutation of the input, never a read out of range -- the epoch
 * check in collect_ordered then decides whether that permutation is believed.
 * Left wins ties, so it is stable.
 */
template <typename Vec, typename Less>
inline void guarded_stable_sort(Vec& v, Vec& tmp, Less less)
{
    const size_t n = v.size();
    if (n < 2) return;
    tmp.resize(n);
    for (size_t w = 1; w < n; w *= 2)
    {
        for (size_t lo = 0; lo < n; lo += 2 * w)
        {
            const size_t mid = ::std::min(lo + w, n), hi = ::std::min(lo + 2 * w, n);
            size_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) tmp[k++] = less(v[j], v[i]) ? v[j++] : v[i++];
            while (i < mid) tmp[k++] = v[i++];
            while (j < hi)  tmp[k++] = v[j++];
        }
        ::std::copy(tmp.begin(), tmp.begin() + n, v.begin());
    }
}
} // namespace detail
// ── RIDListHandle ─────────────────────────────────────────────────────────────
struct RIDListHandle {
    ETCS::Buffer type_name;
    size_t (*count)   (void* self)             = nullptr;
    bool   (*contains)(void* self, RID r)      = nullptr;
    bool   (*remove)  (void* self, RID r)      = nullptr;
    Entity* (*get)    (void* self, RID r)      = nullptr; // New entry
    void (*collect_rids)(void* self, ::std::vector<RID>& out) = nullptr;
    // Type-erased insert — for callers holding only a RIDListHandle, with
    // no T in scope. There are TWO of them because recovering the stored T
    // from what such a caller has takes two different exact routes, and
    // which one is correct depends on what kind of list this is:
    //
    //   insert       — for a CONCRETE-TAG list, where T is the entity's
    //                  most-derived type. Recovers it with getTrueType(),
    //                  the same route the work-func trampolines use.
    //   insert_iface — for an ONTOLOGY FAMILY aggregate, where T is a base
    //                  subobject (Pixels_*, Surface_*). getTrueType() is
    //                  the WRONG pointer there; the caller passes the
    //                  interface pointer ETCS_MAKE_INSTANCE registered,
    //                  which already carries the base adjustment, and the
    //                  cast below is exact by construction.
    //
    // Both conversions happen HERE, inside handle(), which is the last
    // place T is still known -- the string-keyed handle is the erasure
    // boundary on purpose, and what makes trusting a key across it sound
    // is module verification (the ONTOLOGY:/HEADER: manifest hashes, and
    // signage on top of them), not anything recoverable from the pointer.
    
    // Type-erased FACTORY — the one slot that doesn't take `self`, because
    // it doesn't act on an existing list, it mints a new one. Captured at
    // the same type-erasure boundary as every invoke_* slot above (inside
    // handle(), where T is still known), for the one caller that needs to
    // produce a list of THIS tag's concrete type in a DIFFERENT arena
    // without T in scope: Entity::reparentChildrenTo. A handle can't
    // simply be copied across the boundary there — the RIDList<T> object
    // it points at lives in the DYING entity's own local_arena_, so the
    // receiving parent needs its own list, allocated out of ITS arena and
    // dtor-registered there, not an alias to storage that's about to (or,
    // under "coyote time", may later) go away. The returned handle carries
    // this same make_in slot in turn, so a reparented subtree can be
    // reparented again.
    RIDListHandle (*make_in)(MemoryArena& arena, const char* name) = nullptr;
    
    void (*insert)      (void* self, RID r, Entity* e) = nullptr;
    void (*insert_iface)(void* self, RID r, void* iface) = nullptr;
    // The stored pointer as it is actually held -- a T, type-erased to
    // void*. A caller that knows the family (and therefore T) casts it
    // straight back; one that does not uses get() and receives the Entity*
    // upcast instead. Two views of one pointer, never two storage meanings.
    void* (*get_iface)  (void* self, RID r)  = nullptr;
    // APPENDED, never inserted: this struct is stored by value in ridMap and
    // in every entity's typed_children_, and it crosses the DSO boundary --
    // so a slot added in the middle shifts every later one, and a single
    // translation unit built against the older layout calls the wrong
    // function pointer through a valid-looking `self`. New slots go here, at
    // the end, after `self`.
    //
    // collect_rids_ordered falls back to collect_rids' own arbitrary order
    // for a list whose pointee declares no relation -- an unordered list
    // asked for an order gets the honest answer (whatever order it has)
    // rather than a failure, because "no defined order" is a legitimate
    // state for most lists here and not something a caller should special-case.
    void* self                                 = nullptr;
    void (*collect_rids_ordered)(void* self, ::std::vector<RID>& out) = nullptr;
    void (*reorder)(void* self) = nullptr;
    /*
 * NULL WHEN THE POINTEE DECLARES NO ORDER, and that is the signal rather than
 * an oversight: a search needs something to bisect on, and the only thing this
 * list is sorted by is the Orderable relation. A list without one is not
 * "searchable but slow", it has no search key at all, so the slot is absent and
 * a caller can tell the difference. See RIDList::search_ordered.
 *
 * The key is passed as const void* because this is the type-erasure boundary;
 * the lambda that fills this slot casts it back to the pointee type it was
 * generated for. What guarantees the cast is the CALLER NAMING THE LIST: an
 * order search is only meaningful inside one concrete type's list (the
 * comparison belongs to that type), so it is reached by a qualified
 * "Provider:Type" and naming that is naming the exemplar's type.
 */
    void (*search_ordered)(void* self, const void* key, ::std::vector<RID>& out) = nullptr;
    // The same search with the exemplar named by RID -- fully type-erased on
    // both sides, so a caller that cannot name the pointee can still ask.
    // See RIDList::search_ordered_by.
    void (*search_ordered_by)(void* self, RID exemplar, ::std::vector<RID>& out) = nullptr;
    size_t  invoke_count()    const { return count(self);             }
    bool    invoke_contains(RID r) const { return contains(self, r);       }
    bool    invoke_remove(RID r)   const { return remove(self, r);         }
    Entity* invoke_get(RID r)      const { return get(self, r);            } 
    void    invoke_collect_rids(::std::vector<RID>& out)  const { return collect_rids(self, out); }
    void    invoke_collect_rids_ordered(::std::vector<RID>& out) const { return collect_rids_ordered(self, out); }
    void    invoke_reorder() const { reorder(self); }
    void    invoke_insert(RID r, Entity* e) const { insert(self, r, e); }
    void    invoke_insert_iface(RID r, void* iface) const { insert_iface(self, r, iface); }
    void*   invoke_get_iface(RID r)        const { return get_iface(self, r);   }
    RIDListHandle invoke_make_in(MemoryArena& arena, const char* name) const { return make_in(arena, name); }
    // Returns whether this list can be searched at all -- see search_ordered.
    bool    invoke_searchable() const { return search_ordered != nullptr; }
    void    invoke_search_ordered(const void* key, ::std::vector<RID>& out) const
            { if (search_ordered) search_ordered(self, key, out); }
    void    invoke_search_ordered_by(RID exemplar, ::std::vector<RID>& out) const
            { if (search_ordered_by) search_ordered_by(self, exemplar, out); }
};

// ── RIDList<T> ────────────────────────────────────────────────────────────────
template<typename T>
struct RIDList {
    // Define the map type using your ArenaAllocator
    // Note: unordered_map allocates node types, so we must provide the allocator 
    // for the internal pair type.
    static_assert(::std::is_pointer_v<T> && ::std::is_base_of_v<Entity, ::std::remove_pointer_t<T>>,
                  "RIDList<T> requires T to be a pointer to a type derived from Entity.");
    // Stores T, not Entity*. The list is genuinely typed at its own local
    // provider -- inside the module that declared it, where T is a real
    // type -- and only the RIDListHandle above is string-keyed and erased.
    //
    // The direction matters and is why this works: T -> Entity* is a
    // derived-to-virtual-base UPCAST, which the compiler adjusts, so the
    // erased get() below can still hand out an Entity* safely. The reverse,
    // recovering T from a stored Entity*, is a downcast FROM a virtual base
    // and is ill-formed -- which is exactly what getTrueType() and
    // getInterfacePointer() exist to route around, and why the two insert
    // slots on the handle take the routes they do.
    using MapType = ::std::unordered_map<
        RID,
        T,
        ::std::hash<RID>,
        ::std::equal_to<RID>,
        ArenaAllocator<::std::pair<const RID, T>>
    >;
    MapType entities;

    /*
     * WHEN EACH MEMBER ARRIVED, and the one order every read of this list
     * starts from. `entities` is a hash map, and a hash map's walk order is
     * the hash function's and the bucket count's -- the standard library's
     * business, not the program's, and libstdc++ and libc++ do not agree. A
     * read that enumerated it directly, or sorted from it and let ties keep
     * "the order they already had", was handing the runtime an order that
     * could differ between a native build and a browser for the same script:
     * siblings a Causal container interacts in turn, members whose RIDs feed a
     * hash. The arrival number is the program's own fact -- the order the
     * script attached them -- and it is total, so every enumeration below is
     * a function of what was inserted and in what order, nothing else.
     */
    using SeqMap = ::std::unordered_map<
        RID,
        uint64_t,
        ::std::hash<RID>,
        ::std::equal_to<RID>,
        ArenaAllocator<::std::pair<const RID, uint64_t>>
    >;
    SeqMap   arrived_;
    uint64_t next_arrival_ = 0;

    /*
 * THE ORDERED VIEW -- a cache, and everything about it follows from that.
 *
 * Rebuilt on read, only when stale. A tree that is drawn every frame and
 * changes once a second sorts once a second, not sixty times; a list
 * nobody reads in order never sorts at all.
 *
 * STALE IS SET AT THE SEAMS. insert and remove below are where a hash is
 * pushed into or pulled out of this list, and both mark it -- so
 * membership changes need no cooperation from anyone. The case they do
 * NOT cover is the key moving while membership stays put, which no
 * container can observe, and which is the entire reason an ordered
 * container keyed on mutable state is a bug rather than a speedup. That
 * one is Orderable_::Reorder()'s job (ontology/Orderable.h), reaching
 * this through the handle's own reorder slot.
 *
 * mutable, and rebuilt from a const read: the cache is not part of the
 * list's value. Two lists holding the same entities are the same list
 * whether or not either has been read in order yet.
 *
 * Arena-allocated like the map, and cleared rather than freed, so the
 * vector reaches its high-water mark once and stops allocating -- which
 * matters, because a bump arena does not reclaim, and a view that
 * reallocated on every rebuild would grow the arena forever.
 */
    using OrderVec = ::std::vector<RID, ArenaAllocator<RID>>;
    mutable OrderVec ordered_;
    mutable OrderVec scratch_;      // the merge's other half, kept like ordered_
    mutable bool     ordered_stale_ = true;

    // Initialize the map with the singleton arena
    RIDList()
        : entities(ArenaAllocator<::std::pair<const RID, T>>(&MemoryArena::getInstance()))
        , arrived_(ArenaAllocator<::std::pair<const RID, uint64_t>>(&MemoryArena::getInstance()))
        , ordered_(ArenaAllocator<RID>(&MemoryArena::getInstance()))
        , scratch_(ArenaAllocator<RID>(&MemoryArena::getInstance())) {}
    // Entity-local variant — used by Entity::addTag<T> so typed children are
    // allocated out of the owning entity's local arena instead of the global
    // singleton, and get torn down with it.
    explicit RIDList(MemoryArena& arena)
        : entities(ArenaAllocator<::std::pair<const RID, T>>(&arena))
        , arrived_(ArenaAllocator<::std::pair<const RID, uint64_t>>(&arena))
        , ordered_(ArenaAllocator<RID>(&arena))
        , scratch_(ArenaAllocator<RID>(&arena)) {}
    void insert(RID rid, T entity) {
        entities[rid] = entity;
        arrived_.try_emplace(rid, next_arrival_++);   // a re-insert keeps its place
        ordered_stale_ = true;
    }
    void remove(RID rid) {
        entities.erase(rid);
        arrived_.erase(rid);
        ordered_stale_ = true;
    }

    // The members in arrival order: the enumeration every read starts from.
    void collect_arrived(::std::vector<RID>& out) const {
        ::std::vector<::std::pair<uint64_t, RID>> by_arrival;
        by_arrival.reserve(entities.size());
        for (auto const& kv : entities) {
            auto a = arrived_.find(kv.first);
            by_arrival.emplace_back(a != arrived_.end() ? a->second : ~uint64_t(0), kv.first);
        }
        ::std::sort(by_arrival.begin(), by_arrival.end());
        for (auto const& [seq, rid] : by_arrival) out.push_back(rid);
    }

    // The explicit seam. Marks, never sorts -- a burst of reorders before
    // one ordered read costs exactly one rebuild.
    void reorder() const { ordered_stale_ = true; }

    /*
 * Ordered enumeration. The relation is the pointee's own operator<, so a
 * list of BoxNode* orders by what BoxNode means by less-than, and a list
 * whose pointee declares nothing gets its entries in the order they
 * arrived (arrived_) -- a defined order, since the map's own is nobody's.
 *
 * stable_sort, so entries the relation calls equivalent keep the order
 * they already had instead of permuting between rebuilds for no reason.
 *
 * A null stored pointer sorts first rather than being dereferenced. It
 * should not be there at all, but a comparison is the wrong place to
 * discover that.
 */
    /*
 * A SORT SEES ONE COMMITTED STATE OR IS NOT BELIEVED. The relation reads the
 * members' live tag state (a hidden drawable loses), and a tag change commits
 * on its module's ordering thread while this sorts on the reader's. The
 * members' hash epochs are read before and after: every funnel bumps them
 * (Entity::markStateChange), so equal epochs mean the sort straddled no
 * commit -- the same before-or-after a Causal entity's rows give a reader
 * (ontology/CausalBase.h). A sort that did straddle one is sorted again, a
 * bounded number of times; past that it is returned but not cached, and the
 * next read tries again. A commit still between its write and its bump when
 * the epochs are checked is caught one read later instead: its own mark
 * (reorderTypedChild) waits on the parent's lock this read holds, and lands
 * after it.
 */
    void collect_ordered(::std::vector<RID>& out) const {
        if constexpr (detail::has_ordered_pointee_v<T>) {
            if (ordered_stale_) {
                // From arrival order, so what the stable sort keeps for
                // equivalent members is the order the script attached them.
                ordered_.clear();
                ordered_.reserve(entities.size());
                {
                    ::std::vector<RID> arrived;
                    collect_arrived(arrived);
                    ordered_.insert(ordered_.end(), arrived.begin(), arrived.end());
                }
                auto less = [this](RID a, RID b) {
                    T ea = get_typed(a);
                    T eb = get_typed(b);
                    if (!ea || !eb) return ea != nullptr ? false : eb != nullptr;
                    return *ea < *eb;
                };
                auto epochs = [this](::std::vector<uint32_t>& e) {
                    e.clear();
                    for (auto const& kv : entities) e.push_back(kv.second ? kv.second->hashEpoch() : 0u);
                };
                constexpr int kPasses = 3;
                ::std::vector<uint32_t> before, after;
                bool settled = false;
                for (int pass = 0; pass < kPasses && !settled; ++pass) {
                    epochs(before);
                    detail::guarded_stable_sort(ordered_, scratch_, less);
                    epochs(after);
                    settled = (before == after);
                }
                ordered_stale_ = !settled;
            }
            out.insert(out.end(), ordered_.begin(), ordered_.end());
        } else {
            collect_arrived(out);   // no relation: the order they arrived in
        }
    }
    /*
 * EQUAL RANGE, NOT A FIND, and that is forced by what the relation means.
 *
 * Orderable derives == from < , so equality here is EQUIVALENCE -- "neither
 * precedes the other" -- and two different entities with the same standing
 * compare equal (ontology/Orderable.h says so in as many words, and adds that
 * identity is the RID and only the RID). So a search on the order key can
 * legitimately match many entities, and returning one of them would be picking
 * arbitrarily from a set the caller asked for. The result is a range.
 *
 * It comes back in the order stable_sort left it, so a caller reading the
 * matches gets them in the list's own order rather than a permutation that
 * changes between rebuilds.
 *
 * O(log n) against the sorted vector, which is the whole reason this belongs to
 * Orderable and could not be offered generally: `entities` is an unordered_map,
 * so a search by RID is already a hash lookup and bisecting it would be both
 * slower and impossible -- ordered_ is not sorted by RID. The only thing this
 * list can bisect on is the thing it was sorted by.
 *
 * collect_ordered first, because it is what rebuilds a stale order. Searching a
 * stale index is the one way this could quietly return the wrong range.
 */
    void search_ordered(const ::std::remove_pointer_t<T>& key, ::std::vector<RID>& out) const {
        if constexpr (detail::has_ordered_pointee_v<T>) {
            ::std::vector<RID> ord;
            collect_ordered(ord);
            auto lo = ::std::lower_bound(ord.begin(), ord.end(), key,
                [this](RID a, const ::std::remove_pointer_t<T>& k) {
                    T ea = get_typed(a);
                    return ea ? (*ea < k) : true;     // nulls sort first, as in collect_ordered
                });
            auto hi = ::std::upper_bound(lo, ord.end(), key,
                [this](const ::std::remove_pointer_t<T>& k, RID b) {
                    T eb = get_typed(b);
                    return eb ? (k < *eb) : false;
                });
            out.insert(out.end(), lo, hi);
        } else {
            (void)key; (void)out;
        }
    }

    /*
 * THE SAME SEARCH, WITH THE EXEMPLAR NAMED BY RID -- and this is the form most
 * callers actually want.
 *
 * "Everything that stands where THIS one stands" needs no exemplar to be
 * constructed and, more to the point, needs the concrete type in scope NOWHERE
 * outside this list. The list already holds the pointee, so it looks the
 * exemplar up itself and hands it to the comparison. A caller with only a RID
 * and an Entity* -- a loader, another module, a script verb -- can ask an
 * ordering question about a type it cannot name.
 *
 * That is a real gap this closes rather than a convenience. With only the
 * reference overload, an order search was reachable exclusively from code that
 * had the leaf type compiled in, which is the owning module and nothing else.
 * The relation is the type's, but the QUESTION is not, and there was no reason
 * the question should have been confined to it.
 *
 * The exemplar is included in its own result: it stands where it stands.
 */
    void search_ordered_by(RID exemplar, ::std::vector<RID>& out) const {
        if constexpr (detail::has_ordered_pointee_v<T>) {
            T e = get_typed(exemplar);
            if (!e) return;      // not in this list: no standing to match
            search_ordered(*e, out);
        } else {
            (void)exemplar; (void)out;
        }
    }

    bool contains(RID rid) const {
        return entities.find(rid) != entities.end();
    }
    // Upcast on the way out -- see MapType's comment. Callers that know T
    // (i.e. anything inside the type's own module) use get_typed instead
    // and skip the adjustment entirely.
    Entity* get(RID rid) const {
        auto it = entities.find(rid);
        return (it != entities.end()) ? static_cast<Entity*>(it->second) : nullptr;
    }
    T get_typed(RID rid) const {
        auto it = entities.find(rid);
        return (it != entities.end()) ? it->second : nullptr;
    }
    size_t count() const { return entities.size(); }
    RIDListHandle handle(const char* name) {
            RIDListHandle h;
            h.type_name.writeString(name);
            h.count = [](void* self) -> size_t {
                return static_cast<RIDList<T>*>(self)->count();
            };
            h.contains = [](void* self, RID r) -> bool {
                return static_cast<RIDList<T>*>(self)->contains(r);
            };
            h.remove = [](void* self, RID r) -> bool {
                auto* list = static_cast<RIDList<T>*>(self);
                const bool erased = list->entities.erase(r) > 0;
                list->arrived_.erase(r);
                // The erased path is a seam too -- destroyImpl pulls RIDs out
                // through this slot, not through remove() above.
                if (erased) list->reorder();
                return erased;
            };
            h.get = [](void* self, RID r) -> Entity* {
                return static_cast<RIDList<T>*>(self)->get(r);
            };
            h.collect_rids = [](void* self, ::std::vector<RID>& out) {
                static_cast<RIDList<T>*>(self)->collect_arrived(out);
            };
            h.collect_rids_ordered = [](void* self, ::std::vector<RID>& out) {
                static_cast<RIDList<T>*>(self)->collect_ordered(out);
            };
            h.search_ordered = [](void* self, const void* key, ::std::vector<RID>& out) {
                static_cast<RIDList<T>*>(self)->search_ordered(
                    *static_cast<const ::std::remove_pointer_t<T>*>(key), out);
            };
            h.search_ordered_by = [](void* self, RID exemplar, ::std::vector<RID>& out) {
                static_cast<RIDList<T>*>(self)->search_ordered_by(exemplar, out);
            };
            h.reorder = [](void* self) {
                static_cast<RIDList<T>*>(self)->reorder();
            };
            // getTrueType(): correct here because a concrete-tag list's T IS
            // the entity's most-derived type. A family aggregate must NOT
            // come through this slot -- see insert_iface below.
            h.insert = [](void* self, RID r, Entity* e) {
                if (!e) return;
                static_cast<RIDList<T>*>(self)->insert(r, static_cast<T>(etcs_true_type(e)));
            };
            // The caller supplies the already-adjusted base subobject
            // address (getInterfacePointer), so this cast adds no
            // adjustment of its own and cannot be wrong for the family the
            // pointer came from.
            h.insert_iface = [](void* self, RID r, void* iface) {
                if (!iface) return;
                static_cast<RIDList<T>*>(self)->insert(r, static_cast<T>(iface));
            };
            h.get_iface = [](void* self, RID r) -> void* {
                return static_cast<void*>(static_cast<RIDList<T>*>(self)->get_typed(r));
            };
            // Captureless (so it converts to a plain function pointer, same
            // as every lambda above) and deliberately ignores `this` — the
            // new list is empty by construction; the caller migrates RIDs
            // across via invoke_insert/invoke_remove itself, one at a time,
            // rather than this bulk-copying a map whose entries the caller
            // then has to reconcile against what it actually moved.
            //
            // allocate<RIDList<T>> hits MemoryArena's non-trivially-
            // destructible branch (MapType member), so the new list gets a
            // DestructorRecord in `arena` — it tears down with the
            // receiving parent, exactly as the original did with the
            // original parent. The inner RIDList(MemoryArena&) ctor then
            // routes the map's own node storage through that same arena's
            // ArenaAllocator, so nothing about the reparented tag still
            // points into the dying entity's local_arena_.
            h.make_in = [](MemoryArena& arena, const char* name) -> RIDListHandle {
                return arena.allocate<RIDList<T>>(arena)->handle(name);
            };
            h.self = this;
            return h;
        }
    };
} // namespace ETCS
#endif // RIDLIST_H__
