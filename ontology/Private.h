#ifndef SUPERTYPE_PRIVATE_H__
#define SUPERTYPE_PRIVATE_H__

#include "../core_defs.h"
#include <string>

// ---------------------------------------------------------------
// Private
// ---------------------------------------------------------------
//
// READABLE ONLY INSIDE THIS LOADER: one etcs installation as it runs, a
// browser sandbox being its own. A claim about the medium, not the use --
// Local says "never crosses an edge", which a volatile or origin-wide
// store can say and still not keep a secret. Between runs the medium is the
// OS keystore (core/Keystore.h; DatabaseProvider::Keystore).
//
// WHAT A PRIVATE TYPE OWES:
//   - its material never reaches the value surface (no values_, no
//     CaptureState), so no hash, record or reflection carries it;
//   - it is never bridged (MirrorBuffer::localFrame);
//   - it is wiped when it goes.
//
// A script never carries the material: a line names the entity, and the
// edge that needs the bytes (a Foreign leaf) asks for them -- Reveal on the
// way out, Receive on the way in -- after the trace has seen the line.
//
//   Holds    is there material here
//   Wipe     forget it, now
//   Reveal   the material, for the edge writing it out
//   Receive  material from the edge; false if it is not this type's form
class Private_ : virtual public ETCS::Entity
{
public:
    virtual ~Private_() = default;

    virtual bool Holds() const = 0;
    virtual void Wipe() = 0;
    virtual bool Reveal(std::string& out) const = 0;
    virtual bool Receive(const std::string& in) = 0;
};

#endif // SUPERTYPE_PRIVATE_H__
