#ifndef RIDLIST_H__
#define RIDLIST_H__
#include <unordered_map>
#include "Buffer.h"
#include "ArenaAllocator.h"
namespace ETCS {
using RID = ETCS_RID_SIZE;
class Entity;
// ── RIDListHandle ─────────────────────────────────────────────────────────────
struct RIDListHandle {
    ETCS::Buffer type_name;
    size_t (*count)   (void* self)             = nullptr;
    bool   (*contains)(void* self, RID r)      = nullptr;
    bool   (*remove)  (void* self, RID r)      = nullptr;
    Entity* (*get)    (void* self, RID r)      = nullptr; // New entry
    void (*collect_rids)(void* self, std::vector<RID>& out) = nullptr;
    // Type-erased insert — added so callers holding only a RIDListHandle
    // (no T in scope) can insert an already-constructed Entity* without
    // ever needing to cast back to the concrete RIDList<T>*. Takes Entity*
    // rather than T*, matching RIDList<T>::insert's own signature below —
    // storage is always Entity*, regardless of what T the list was
    // templated on.
    
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
    
    void (*insert)    (void* self, RID r, Entity* e) = nullptr;
    void* self                                 = nullptr;
    size_t  invoke_count()    const { return count(self);             }
    bool    invoke_contains(RID r) const { return contains(self, r);       }
    bool    invoke_remove(RID r)   const { return remove(self, r);         }
    Entity* invoke_get(RID r)      const { return get(self, r);            } 
    void    invoke_collect_rids(std::vector<RID>& out)  const { return collect_rids(self, out); }
    void    invoke_insert(RID r, Entity* e) const { insert(self, r, e); }
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
    using MapType = std::unordered_map<
        RID, 
        Entity*, 
        std::hash<RID>, 
        std::equal_to<RID>, 
        ArenaAllocator<std::pair<const RID, Entity*>>
    >;
    MapType entities;
    // Initialize the map with the singleton arena
    RIDList() : entities(ArenaAllocator<std::pair<const RID, Entity*>>(&MemoryArena::getInstance())) {}
    // Entity-local variant — used by Entity::addTag<T> so typed children are
    // allocated out of the owning entity's local arena instead of the global
    // singleton, and get torn down with it.
    explicit RIDList(MemoryArena& arena)
        : entities(ArenaAllocator<std::pair<const RID, Entity*>>(&arena)) {}
    void insert(RID rid, Entity* entity) {
        entities[rid] = entity;
    }
    void remove(RID rid) {
        entities.erase(rid);
    }
    bool contains(RID rid) const {
        return entities.find(rid) != entities.end();
    }
    Entity* get(RID rid) const {
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
            h.insert = [](void* self, RID r, Entity* e) {
                static_cast<RIDList<T>*>(self)->insert(r, e);
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
