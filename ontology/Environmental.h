#ifndef SUPERTYPE_ENVIRONMENTAL_H__
#define SUPERTYPE_ENVIRONMENTAL_H__


#include "../core_defs.h"
#include <cstring>
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
// happen (core/Provenance.h) and compacts them into a script
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
// WHAT THE SCRIPT CANNOT SAY -- state held in members rather than on
// the tag surface -- goes in the EnvironmentState a type captures:
// named values, and ONLY that: the script puts the tags back, these
// put the rest. Replaying ETCS actions is deterministic by the rules
// of ETCS scripts, so anything that was not (the clock, input, a far
// node's answer) belongs here as its result.
//
//   CaptureState   the named values, now.
//   RebuildLocal   after this entity's script has run HERE: put them back.
//   ReflectRemote  after the far node's script has built this reflection:
//                  put back what a reflection should show.
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

class Environmental_ : virtual public ETCS::Entity
{
public:
    virtual ~Environmental_() = default;

    virtual void        CaptureState(ETCS::EnvironmentState& out) const = 0;
    virtual bool        RebuildLocal(const ETCS::EnvironmentState& state) = 0;
    virtual bool        ReflectRemote(const ETCS::EnvironmentState& state) = 0;
    virtual std::string MigrateTo() const = 0;
};

#endif // SUPERTYPE_ENVIRONMENTAL_H__
