#ifndef SUPERTYPE_CONNECTIONSTATE_H__
#define SUPERTYPE_CONNECTIONSTATE_H__


#include "../core_defs.h"
#include "Ephemeral.h"

// ConnectionState_ — supertype for a single open connection's runtime state.
// Sits in the Ephemeral branch (not Parser_) because a connection is a
// stateful, addressable, mutable object across its lifetime — not a protocol
// transform. It is spawned as a CHILD of the entity that accepted the
// connection (its parser/parent), via Entity::addTag<T>(), so its lifetime
// is naturally bound to that parent: as long as the parent (e.g. an
// HTTPParser instance) is alive, the child ConnectionState's RID stays valid.

// A ConnectionState does not own its own protocol parser — it drives parsing
// through its parent entity (reachable via Entity::getParentEntity()), since
// they are leaf tags on the same parent and their lifetimes are already in
// sync by construction. This avoids duplicating parser state per connection
// when the parent already has the means to parse.

// Serving page content is expressed by holding a reference (RID) to an
// HtmlPage_ entity — see HtmlPage.h. Swapping which page a connection serves
// is just retargeting that reference, addressable from any .etcs script that
// can reach this ConnectionState's RID (e.g. via `list NetworkProvider::ConnectionState`).

class ConnectionState_ : virtual public ETCS::Entity
{
public:
    virtual ~ConnectionState_() = default;

    // The OS-level fd this connection is communicating over. Exposed so
    // work functions (SetPage, Close, etc.) operating on this entity by RID
    // can reach the underlying transport without needing a MirrorBuffer
    // already in scope.
    virtual int  GetClientFd() const = 0;
    virtual void SetClientFd(int fd) = 0;

    // The RID of the HtmlPage_ entity currently being served on this
    // connection. 0 means "no page assigned yet" (caller should default).
    virtual ETCS::RID GetPageRID() const  = 0;
    virtual void       SetPageRID(ETCS::RID rid) = 0;

    // True while the connection is open and the request/response cycle for
    // it is still in flight. False once the connection has been closed
    // (normally or due to error) — a signal to any consumer/script that this
    // ConnectionState's RID is now stale.
    virtual bool IsConnectionOpen() const = 0;
};

#endif // SUPERTYPE_CONNECTIONSTATE_H__
