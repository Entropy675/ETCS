#ifndef SUPERTYPE_DIRECTORY_H__
#define SUPERTYPE_DIRECTORY_H__


#include "../core_defs.h"
#include <string>

// ---------------------------------------------------------------
// Directory
// ---------------------------------------------------------------
//
// Who can be reached, by name. A lobby is one: hosts say they are
// there and what they are for, and anyone who can see the directory
// can list them and go to one. The chess lobby list and the paint
// share list were each this, written twice.
//
// AN ENTRY LIVES AS LONG AS ITS ADVERTISER IS REACHABLE. An advert
// that arrived over a link is withdrawn when that link closes -- the
// liveness is the edge's, not a poll or a timeout the directory keeps
// -- and one made locally lasts until it is withdrawn. So a listing
// never names a host nobody can reach.
//
// A DIRECTORY HANDS OUT REFERENCES, NOT SESSIONS. `info` is where to go
// (an address, a room); what happens there is between the guest and
// that host, over its own link. Being listed confers nothing: reaching
// a host is still subject to what that host publishes and the
// authority layer guarding it (ontology/Remote.h).

class Directory_ : virtual public ETCS::Entity
{
public:
    virtual ~Directory_() = default;

    virtual bool        Advertise(const std::string& name, const std::string& kind,
                                  const std::string& info) = 0;
    virtual bool        Withdraw(const std::string& name) = 0;
    // "name kind info" per line; an empty kind lists every kind.
    virtual std::string Listing(const std::string& kind) const = 0;
};

#endif // SUPERTYPE_DIRECTORY_H__
