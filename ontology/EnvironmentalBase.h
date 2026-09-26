#ifndef BASE_ENVIRONMENTAL_H__
#define BASE_ENVIRONMENTAL_H__
#include "Environmental.h"

#include <algorithm>
#include <mutex>

// Claiming the family is what turns provenance on for the entity: once it is
// in the graph, every action that changes its tag surface is recorded here
// (IWireEnvironmental, core/Provenance.h) -- the record is the family's, so
// nothing else pays for it.
//
// CaptureState and MigrateTo default to "nothing off the surface" and "no
// renames"; RebuildLocal and ReflectRemote are the type's to say, because
// how an entity comes back -- here, or as a reflection -- is its own.
// Closing is nothing unless the type keeps something (Persistence).
ETCS_SUPERTYPE_BASE(Environmental)
{
    ETCS_MAKE_INSTANCE(Environmental)
    ETCS_DISPATCH_METHOD(bool, RebuildLocal,  (const ETCS::EnvironmentState&, state));
    ETCS_DISPATCH_METHOD(bool, ReflectRemote, (const ETCS::EnvironmentState&, state));

    void CaptureState(ETCS::EnvironmentState& out) const override
    { static_cast<const Derived*>(this)->CaptureStateConcrete(out); }
    std::string MigrateTo() const override
    { return static_cast<const Derived*>(this)->MigrateToConcrete(); }

    void        CaptureStateConcrete(ETCS::EnvironmentState&) const {}
    std::string MigrateToConcrete() const { return {}; }

    // ── IWireEnvironmental ────────────────────────────────────────────────
    // One record per action: a second change inside the frame the last record
    // belongs to joins it.
    void RecordEffect(const ETCS::ActionFrame* f, const std::string& key, bool created) override
    {
        std::lock_guard<std::mutex> lock(record_mu_);
        if (f && !record_.empty() && record_.back().frame == f->id)
        {
            (created ? record_.back().created : record_.back().removed).push_back(key);
            return;
        }
        ETCS::ActionRecord r;
        r.seq   = ++record_seq_;
        r.frame = f ? f->id : 0;
        if (f) r.line = f->line;
        (created ? r.created : r.removed).push_back(key);
        record_.push_back(std::move(r));
    }
    std::vector<ETCS::ActionRecord> ActionLog() const override
    {
        std::lock_guard<std::mutex> lock(record_mu_);
        return record_;
    }
    // After a capture of the record up to `upto`: what is left of that is what
    // the capture kept. It rebuilds the same surface, and everything dropped
    // is history the surface no longer shows -- the snapshot frames' to keep,
    // not this record's. Later records are the next capture's.
    void KeepActions(const std::vector<uint64_t>& seqs, uint64_t upto) override
    {
        std::lock_guard<std::mutex> lock(record_mu_);
        std::vector<ETCS::ActionRecord> kept;
        for (auto& r : record_)
            if (r.seq > upto || std::find(seqs.begin(), seqs.end(), r.seq) != seqs.end())
                kept.push_back(std::move(r));
        record_.swap(kept);
    }
    void NoteName(const std::string& name) override
    { std::lock_guard<std::mutex> lock(record_mu_); script_name_ = name; }
    std::string ScriptName() const override
    { std::lock_guard<std::mutex> lock(record_mu_); return script_name_; }
    void Closing() override {}

private:
    mutable std::mutex              record_mu_;
    std::vector<ETCS::ActionRecord> record_;
    uint64_t                        record_seq_ = 0;
    std::string                     script_name_;
};

#endif // BASE_ENVIRONMENTAL_H__
