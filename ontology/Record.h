#ifndef SUPERTYPE_RECORD_H__
#define SUPERTYPE_RECORD_H__


#include "../core_defs.h"
#include <cstdint>
#include <string>

// ---------------------------------------------------------------
// Record
// ---------------------------------------------------------------
//
// What happened, in the order it happened, as one authority decided
// it: numbered lines, each chained by hash onto everything before it.
// The chess pair's relay and the paint session's op log were each
// this, written twice.
//
// ONE WRITER ORDERS IT. Append is the ordering act, and it runs where
// the record lives -- a guest appends by running the verb on its
// surface of the host's record (ontology/Remote.h), so the host's
// runtime puts every guest's line in one order and nobody else's
// copy decides anything. The line's author is who appended it; over
// a link, the link's identity, not a field the author wrote.
//
// THE CHAIN IS THE AGREEMENT. Two copies that report the same head --
// sequence and chain -- hold the same history; a copy that does not
// has missed or invented a line, and rebuilds from the record rather
// than arguing with it. Whatever a participant derives from the
// lines (a board, a document) is an ordinary entity, and the core
// already says whether two of those are in the same state (the
// runtime hash); the record only has to say they were fed the same.
//
// Reading is Since: the lines from a sequence on, as many as fit. A
// follower that wants them as they happen streams them instead
// (the leaf's own Follow, NetworkProvider/Ledger.h).
//
// A RECORD KEEPS ITS LINES FROM THE LAST CHECKPOINT ON. A line that
// states everything a reader needs (a page, a board) makes the lines
// before it history nobody replays, and a record that keeps them only
// grows with use. The writer says so -- Checkpoint(seq) -- and the
// record drops what came before `seq`. A reader asking from before it
// is answered from the checkpoint instead, told first where that is
// and what the chain was there ("~ <seq> <chain hex>"), and it starts
// its own chain from that rather than from zero.
//
// Checkpoint is the ordering authority's: a guest cannot shorten the
// history everyone else reads.

class Record_ : virtual public ETCS::Entity
{
public:
    virtual ~Record_() = default;

    // The new line's sequence, or UINT64_MAX when refused.
    virtual uint64_t    Append(const std::string& author, const std::string& line) = 0;
    // "<next seq> <chain hex>"
    virtual std::string Head() const = 0;
    // "<from> <to> <chain hex at to>\n" then "<seq> <author> <line>\n" per line,
    // led by "~ <checkpoint> <chain hex there>\n" when `seq` is before it.
    virtual std::string Since(uint64_t seq, size_t budget) const = 0;
    // Drops the lines before `seq`; false when refused or out of range.
    virtual bool        Checkpoint(uint64_t seq) = 0;
};

#endif // SUPERTYPE_RECORD_H__
