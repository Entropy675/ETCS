#ifndef SUPERTYPE_ENVIRONMENTAL_H__
#define SUPERTYPE_ENVIRONMENTAL_H__


#include "../core_defs.h"
#include "Causal.h"
#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <unordered_map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// ---------------------------------------------------------------
// Environmental
// ---------------------------------------------------------------
//
// An entity whose state can be made present in a frame of reference
// other than the moment it was built in. There are two such frames and
// they are the same operation seen from two sides:
//
//   LOCAL   rebuilt HERE, from its own record -- on a restart, from the
//           store this runtime keeps (DatabaseProvider's Persistence).
//   REMOTE  reflected over THERE, on the far side of a MirrorBuffer --
//           a surface of it in another runtime (ontology/Remote.h).
//
// Either way what rebuilds it is not a copy of its state but the ETCS
// actions that produced it, replayed: the runtime records them as they
// happen, into the family's own record (IWireEnvironmental,
// core/Provenance.h), and compacts them into a script
// (etcs_replay_capture, Entity.h). The script is the same for both
// frames; a local rebuild IS the remote build of the active graph,
// without the boundary. Which frame an instance is in is a fact of the
// graph -- a surface carries a Remote child -- never something it
// claims.
//
// OPT-IN. A type that never claims this family records nothing, costs
// nothing, and can be neither persisted nor reflected: the small fast
// types that live purely in the functional realm stay that way.
//
// WHAT THE SCRIPT CANNOT SAY -- the result of anything that was not an
// action: the clock, input, a far node's answer, a body's rows after a
// thousand interactions -- is ON THE TAG SURFACE TOO, as the value behind
// the tag that names that state (Entity::values_: stored through the
// funnel, or bound by a family and read on demand). The script puts the
// tags back; the values, captured off the surface (etcs_capture_values)
// and handed back to it (etcs_restore_values), put the rest. One surface,
// one capture, one store -- a type keeps nothing in a place of its own.
//
//   RebuildLocal   after this entity's script has run HERE and its values
//                  are back on its surface: anything the type does with
//                  them (most types: nothing).
//   ReflectRemote  after the far node's script has built this reflection
//                  and handed it the values: what a reflection does with
//                  them.
//   MigrateTo      how earlier builds' keys map to this build's, oldest
//                  first: "old=new" per line. The ETCS surface only ever
//                  grows, so an old script still replays; only the named
//                  values can have moved.
//
// THE LOCAL FRAME NEVER CROSSES. What rebuilds an entity here reads this
// runtime's own store, and a MirrorBuffer does not bridge it.

namespace ETCS
{
// Named values a replay script cannot carry. Binary-safe.
struct EnvironmentState
{
    std::vector<std::pair<std::string, std::string>> kv;

    void set(const std::string& k, std::string v)
    {
        for (auto& [key, val] : kv) if (key == k) { val = std::move(v); return; }
        kv.emplace_back(k, std::move(v));
    }
    const std::string* get(const std::string& k) const
    {
        for (auto& [key, val] : kv) if (key == k) return &val;
        return nullptr;
    }
    // Apply a MigrateTo map ("old=new" per line) to the keys.
    void migrate(const std::string& map)
    {
        size_t at = 0;
        while (at < map.size())
        {
            size_t nl = map.find('\n', at);
            if (nl == std::string::npos) nl = map.size();
            const std::string line = map.substr(at, nl - at);
            at = nl + 1;
            const size_t eq = line.find('=');
            if (eq == std::string::npos || eq == 0) continue;
            const std::string from = line.substr(0, eq), to = line.substr(eq + 1);
            for (auto& [key, val] : kv) if (key == from) key = to;
        }
    }
    // [u32 klen][key][u32 vlen][value]... -- how a store keeps it.
    std::string pack() const
    {
        std::string out;
        for (auto& [k, v] : kv)
        {
            const uint32_t kl = static_cast<uint32_t>(k.size()), vl = static_cast<uint32_t>(v.size());
            out.append(reinterpret_cast<const char*>(&kl), 4); out += k;
            out.append(reinterpret_cast<const char*>(&vl), 4); out += v;
        }
        return out;
    }
    bool unpack(const std::string& in)
    {
        kv.clear();
        size_t at = 0;
        while (at < in.size())
        {
            uint32_t kl = 0, vl = 0;
            if (at + 4 > in.size()) return false;
            std::memcpy(&kl, in.data() + at, 4); at += 4;
            if (at + kl + 4 > in.size()) return false;
            std::string k = in.substr(at, kl); at += kl;
            std::memcpy(&vl, in.data() + at, 4); at += 4;
            if (at + vl > in.size()) return false;
            kv.emplace_back(std::move(k), in.substr(at, vl)); at += vl;
        }
        return true;
    }
};
} // namespace ETCS

// IWireEnvironmental first, at offset 0 (core/InterfaceWire.h): the runtime
// records into it and reads it back without knowing this family.
class Environmental_ : public ETCS::IWireEnvironmental, virtual public ETCS::Entity
{
public:
    virtual ~Environmental_() = default;

    virtual bool        RebuildLocal(const ETCS::EnvironmentState& state) = 0;
    virtual bool        ReflectRemote(const ETCS::EnvironmentState& state) = 0;
    virtual std::string MigrateTo() const = 0;
};

// The value surface of any entity, as the state a store or a reflection
// carries -- not only an Environmental one: a body under an Environmental
// root has rows, and they come back with it.
inline void etcs_capture_values(const ETCS::Entity* e, ETCS::EnvironmentState& out)
{
    std::vector<std::pair<std::string, std::string>> kv;
    e->values(kv);
    for (auto& [k, v] : kv) out.set(k, std::move(v));
}
/*
 * A FROZEN READ of a tree: what a store keeps, all of it from one state.
 *
 * A save reads a moving thing. Read live, node by node, a body can step
 * between two reads and a ledger can take a line between the hash and the
 * values, and what is kept is a state that never was -- with a hash that
 * does not describe it. So the read is a copy, made in one window:
 *
 *   - every Causal tree under the root is held by its lock (TreeMutex), in
 *     address order, so its rows are one tick -- they move under that lock
 *     and nowhere else;
 *   - the funnel is not stopped but WATCHED: every flag or value change under
 *     the root bumps the root's hash epoch (markStateChange walks up), so an
 *     epoch that moved across the window means the copy may be torn, and it
 *     is made again (a seqlock on a counter the funnel already keeps);
 *   - the state hash is composed FROM THE COPY (Entity::stateHashWith over
 *     subtreeValueHashOf), never read live beside it, so the number kept is
 *     the values kept -- a bound value that moves without either (a ledger's
 *     line) cannot come between them.
 *
 * Everything after the window -- packing, keys, the database -- reads the
 * copy with nothing held. `inside` runs in the window too: a caller whose
 * other half must be the same state (the replay capture) does it there.
 * `stable` is false when the epoch kept moving through every try.
 *
 * ONLY WHAT MOVED IS READ. Given the last read (`prev`), a node whose hash
 * epoch (every flag or stored value under it) and value digest (its bound
 * values, Entity::valueDigest) both stand where they stood is taken from it,
 * not read again -- the walk still visits it, the copy does not. `copied`
 * counts the nodes read this time; zero means the read is the last one.
 */
struct FrozenNode
{
    ETCS::Entity* e = nullptr;            // identity only: not held once the window closes
    ETCS::RID     rid = 0;
    uint32_t      epoch = 0;              // what it was read at (hashEpoch, valueDigest)
    uint64_t      digest = 0;
    bool          digestible = false;
    uint64_t      vhash = 0;              // Entity::valueHashOf(kv)
    std::string   module, tag;            // what it is, read in the window (getSourceModule/Tag)
    uint64_t      identity = 0;
    bool          environmental = false;
    std::vector<std::string>                         flags;   // state flags, sorted
    std::vector<std::pair<std::string, std::string>> kv;      // the value surface, sorted
    std::vector<size_t>                              kids;    // indices, in the walk's order
};
struct FrozenTree
{
    std::vector<FrozenNode> nodes;        // the hash's walk: pre-order, nodes[0] the root
    uint64_t state_hash = 0;
    bool     stable = false;
    size_t   copied = 0;                  // nodes read this time; the rest came from `prev`
};

inline bool etcs_freeze(ETCS::Entity* root, FrozenTree& out,
                        const std::function<void()>& inside = {}, int tries = 4,
                        FrozenTree* prev = nullptr)
{
    // The last read is consumed: a node that stood still is moved out of it,
    // and handed back if the attempt does not settle, so a retry reads again
    // only what moved.
    std::unordered_map<ETCS::RID, FrozenNode*> last;
    if (prev) { last.reserve(prev->nodes.size()); for (FrozenNode& n : prev->nodes) last[n.rid] = &n; }
    // The walk subtreeValueHash makes (order_canonical), so the per-node
    // order is the hash's.
    auto visit = [](ETCS::Entity* top, bool canonical, const std::function<void(ETCS::Entity*, size_t parent)>& at) {
        struct Frame { ETCS::Entity* n; size_t idx; std::vector<std::pair<ETCS::Buffer, ETCS::RID>> kids; size_t next = 0; ETCS::LifetimeHold hold; };
        std::vector<Frame> stack;
        size_t count = 0;
        auto open = [&](ETCS::Entity* n, size_t parent, ETCS::LifetimeHold&& hold) {
            Frame f; f.n = n; f.idx = count++; f.hold = std::move(hold);
            at(n, parent);
            n->getTypedChildren(f.kids);
            if (canonical) ETCS::etcs_hash_detail::order_canonical(n, f.kids);
            return f;
        };
        stack.push_back(open(top, SIZE_MAX, ETCS::LifetimeHold()));
        while (!stack.empty())
        {
            Frame& f = stack.back();
            if (f.next < f.kids.size())
            {
                const auto [tag, rid] = f.kids[f.next++];
                ETCS::Entity* child = f.n->getTypedChild(tag, rid);
                ETCS::LifetimeHold hold(child);
                if (!hold) continue;
                const size_t parent = f.idx;
                stack.push_back(open(child, parent, std::move(hold)));
                continue;
            }
            stack.pop_back();
        }
    };
    for (int attempt = 0; attempt < tries; ++attempt)
    {
        const uint32_t epoch = root->hashEpoch();
        // The trees to hold: found outside the window (a tree that appears
        // after this bumps the epoch, and the copy is made again).
        std::vector<std::recursive_mutex*> locks;
        visit(root, false, [&locks](ETCS::Entity* n, size_t) {   // which trees: any order
            if (void* c = n->getInterfacePointer(ETCS::Buffer("Causal")))
                locks.push_back(&static_cast<Causal_*>(c)->TreeMutex());
        });
        std::sort(locks.begin(), locks.end());
        locks.erase(std::unique(locks.begin(), locks.end()), locks.end());
        for (auto* m : locks) m->lock();

        out.nodes.clear();
        out.copied = 0;
        std::vector<uint64_t> per_node;
        std::vector<std::pair<size_t, FrozenNode*>> taken;   // (index in out, where it came from)
        visit(root, true, [&out, &per_node, &last, &taken](ETCS::Entity* n, size_t parent) {
            FrozenNode fn;
            const uint32_t ep = n->hashEpoch();
            uint64_t dg = 0;
            const bool digestible = n->valueDigest(dg);
            auto was = last.find(n->getRID());
            if (was != last.end() && was->second->e == n && was->second->rid == n->getRID()
                && was->second->epoch == ep && digestible && was->second->digestible && was->second->digest == dg)
            {
                fn = std::move(*was->second);   // stood still: the last read is this one
                was->second->rid = 0;           // spent, until handed back
                fn.kids.clear();
                taken.emplace_back(out.nodes.size(), was->second);
            }
            else
            {
                fn.e = n;
                fn.rid = n->getRID();
                fn.epoch = ep;
                fn.digest = dg;
                fn.digestible = digestible;
                fn.module = n->getSourceModule().toString();
                fn.tag    = n->getSourceTag().toString();
                fn.identity = n->identityHash();
                fn.environmental = n->isEnvironmental();
                n->stateFlags(fn.flags);
                std::sort(fn.flags.begin(), fn.flags.end());
                n->values(fn.kv);
                fn.vhash = ETCS::Entity::valueHashOf(fn.kv);
                ++out.copied;
            }
            per_node.push_back(fn.vhash);
            if (parent != SIZE_MAX) out.nodes[parent].kids.push_back(out.nodes.size());
            out.nodes.push_back(std::move(fn));
        });
        out.state_hash = root->stateHashWith(ETCS::Entity::subtreeValueHashOf(per_node));
        if (inside) inside();

        for (auto it = locks.rbegin(); it != locks.rend(); ++it) (*it)->unlock();
        out.stable = (root->hashEpoch() == epoch);
        if (out.stable) return true;
        for (auto& [i, from] : taken) { *from = std::move(out.nodes[i]); from->kids.clear(); }
    }
    return false;
}

// The values back onto the surface; how many landed. A key the surface has
// no place for (a flag the script did not put back, a binding this build
// does not have) is skipped, and the count says so.
inline size_t etcs_restore_values(ETCS::Entity* e, const ETCS::EnvironmentState& st)
{
    size_t n = 0;
    for (auto& [k, v] : st.kv) if (e->restoreValue(ETCS::Buffer(k.c_str()), v)) ++n;
    return n;
}

#endif // SUPERTYPE_ENVIRONMENTAL_H__
