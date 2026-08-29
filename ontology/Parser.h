#ifndef SUPERTYPE_PARSER_H__
#define SUPERTYPE_PARSER_H__


#include "../core_defs.h"

// Transform vs Session?
class Parser_ : virtual public ETCS::Entity {
public:
    virtual ~Parser_() = default;

    /**
     * @brief The primary entry point for protocol processing.
     * @param io The MirrorBuffer pair (Consumer side usually)
     * @param ctx Signal context for interrupts/backpressure
     */
    virtual void Parse(ETCS::MirrorBuffer& io, ETCS::SignalContext ctx) = 0;

    // Standard state inspection
    virtual bool Reset() = 0;
};

#endif
