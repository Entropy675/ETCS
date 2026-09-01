#ifndef RIDLIST_H__
#define RIDLIST_H__
#include <unordered_map>
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
// ── RIDListHandle ─────────────────────────────────────────────────────────────
struct RIDListHandle {
    ETCS::Buffer type_name;
    size_t (*count)   (void* self)             = nullptr;
    bool   (*contains)(void* self, RID r)      = nullptr;
    bool   (*remove)  (void* self, RID r)      = nullptr;
    Entity* (*get)    (void* self, RID r)      = nullptr; // New entry
    void (*collect_rids)(void* self, std::vector<RID>& out) = nullptr;
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
    void* self                                 = nullptr;
    size_t  invoke_count()    const { return count(self);             }
    bool    invoke_contains(RID r) const { return contains(self, r);       }
    bool    invoke_remove(RID r)   const { return remove(self, r);         }
    Entity* invoke_get(RID r)      const { return get(self, r);            } 
    void    invoke_collect_rids(std::vector<RID>& out)  const { return collect_rids(self, out); }
    void    invoke_insert(RID r, Entity* e) const { insert(self, r, e); }
    void    invoke_insert_iface(RID r, void* iface) const { insert_iface(self, r, iface); }
    void*   invoke_get_iface(RID r)        const { return get_iface(self, r);   }
    RIDListHandle invoke_make_in(MemoryArena& arena, const char* name) const { return make_in(arena, name); }
};

// ── RIDList<T> ────────────────────────────────────────────────────────────────
template<typename T>
struct RIDList {
    // Define the map type using your ArenaAllocator
    // Note: unordered_map allocates node types, so we must provide the allocator 
    // for the internal pair type.
    static_assert(std::is_pointer_v<T> && std::is_base_of_v<Entity, std::remove_pointer_t<T>>,
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
    using MapType = std::unordered_map<
        RID,
        T,
        std::hash<RID>,
        std::equal_to<RID>,
        ArenaAllocator<std::pair<const RID, T>>
    >;
    MapType entities;
    // Initialize the map with the singleton arena
    RIDList() : entities(ArenaAllocator<std::pair<const RID, T>>(&MemoryArena::getInstance())) {}
    // Entity-local variant — used by Entity::addTag<T> so typed children are
    // allocated out of the owning entity's local arena instead of the global
    // singleton, and get torn down with it.
    explicit RIDList(MemoryArena& arena)
        : entities(ArenaAllocator<std::pair<const RID, T>>(&arena)) {}
    void insert(RID rid, T entity) {
        entities[rid] = entity;
    }
    void remove(RID rid) {
        entities.erase(rid);
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
                return list->entities.erase(r) > 0;
            };
            h.get = [](void* self, RID r) -> Entity* {
                return static_cast<RIDList<T>*>(self)->get(r);
            };
            h.collect_rids = [](void* self, std::vector<RID>& out) {
                auto* list = static_cast<RIDList<T>*>(self);
                for (auto const& [rid, entity] : list->entities) {
                    out.push_back(rid);
                }
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
