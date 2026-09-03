#ifndef BASE_DATABASE_H__
#define BASE_DATABASE_H__
#include "Database.h"

// ---------------------------------------------------------------
// DatabaseBase
// ---------------------------------------------------------------
//
// ONE FAMILY. Locality is not part of it, and never was.
//
// This replaces LocalDatabaseBase and RemoteDatabaseBase, which split the
// family along a line that carried no constraint. `LocalDatabase_` was an
// empty class -- a refinement that added nothing, which by the ontology's own
// rule (§2: refinement adds obligation, it never subtracts) is not a
// refinement. `RemoteDatabase_` added `DisconnectRemote`, which is
// `Database_::CloseConnection` under another name, and a commented-out
// `connectRemote` that `CreateConnection(const Buffer&)` already covered --
// a connection string is a connection string whether it names a path or a
// host. Its Base dispatched nothing at all and said so: "undecided for now".
//
// The four dispatches below are exactly the ones LocalDatabaseBase already
// declared. Nothing about what a database owes has changed; only the number
// of names for it.
//
// WHERE LOCALITY LIVES INSTEAD. It is a property an instance rolls into
// itself, not a constraint the family imposes:
//
//   - `ETCS::Remote` (core/MirrorBuffer.h) is a marker base that flips a
//     type's stream transport to a socket, detected by is_base_of with no
//     macro changes. A database whose rows arrive over a network says so by
//     inheriting it, and StrategyFor does the rest.
//   - `EntityLocale` records the same fact for runtime inspection.
//
// So the transport layer already spans local and remote, and a family
// boundary drawn at locality would be re-deriving an abstraction that exists
// one layer down. What differs between a file on this disk and a server
// across a network is HOW THE BYTES ARRIVE, which is MirrorBuffer's question,
// not what a database must be able to do, which is this one's.
//
// THE DISTINCTION STILL SURFACES -- as two TAGS. A provider is free to export
// `LocalDatabase` and `RemoteDatabase` as separate types, both claiming this
// one family, differing in which engine they wrap and which orthogonal
// families they compose. A script spawning one of them is stating where it
// expects the data to live, which is a causal fact it genuinely knows and
// cares about (durability, sharing, latency). That is an IDENTITY, and
// identities are what tags are for. It is not a CONSTRAINT, and constraints
// are what families are for.

ETCS_SUPERTYPE_BASE(Database)
{
    ETCS_MAKE_INSTANCE(Database)
    ETCS_DISPATCH_METHOD(void, CloseConnection);
    ETCS_DISPATCH_METHOD(void, CreateConnection, (const ETCS::Buffer&,  db));
    ETCS_DISPATCH_METHOD(bool, ExecuteRaw,       (ETCS::Buffer&,        data));
    ETCS_DISPATCH_METHOD(bool, InitializeSchema, (const ETCS::Buffer&,  schema));
};
#endif
