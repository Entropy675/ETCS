#ifndef SUPERTYPE_ANIMATED_H__
#define SUPERTYPE_ANIMATED_H__


#include "../core_defs.h"

/*
 * ── ANIMATED ─────────────────────────────────────────────────────────────
 *
 * SOMETHING THAT HAS TO BE ADVANCED, and a place to say so.
 *
 * This is a CAUSAL relation and not a drawing one: "I am not finished, come
 * back" plus "here is where you come back to". Before it existed, every type
 * that needed it said so by pretending to be something else -- a node in a
 * drawable tree that draws nothing, exists only to be visited by the compose
 * walk, and does its stepping inside its own DrawInto. That is a real
 * relationship smuggled through an unrelated family, and it costs exactly what
 * smuggling costs: the type has to be a Drawable it is not, it has to be a
 * CHILD of a tree it has no business being in, and the thing that advances it
 * is whatever happens to walk that tree rather than whatever owns the clock.
 *
 * SO IT IS ITS OWN FAMILY, with no relation to any other. An entity claims
 * Animated and is advanced; nothing else about it has to be true. A tween on a
 * value, a physics step, a retry backoff and a cursor blink are the same
 * relation to the runtime and different in every other way.
 *
 * TWO QUESTIONS, AND THE FIRST ONE IS WHY THIS IS CHEAP. Animating() is asked
 * before every step, so a settled animation costs one virtual call per visit
 * and nothing else -- no allocation, no timer, no thread. Whatever drives the
 * clock can therefore visit everything unconditionally and let each one say
 * whether it has anywhere left to go, which is the only arrangement where
 * "somebody else decides when" does not also mean "somebody else has to know
 * what".
 *
 * WHAT DRIVES IT IS NOT STATED HERE, on purpose. A frame edge is the obvious
 * driver and is what RenderProvider uses (Surface::ConsumeFrames), but a
 * headless session with no surface at all can advance the same family from its
 * own loop, and a test can advance it by hand. The family says an entity is
 * advanceable; who advances it is a property of the session.
 *
 * THE INTERVAL IS MEASURED, NOT COUNTED, and AnimatedBase is where that is
 * done -- see it for why that is what makes more than one driver safe.
 */
class Animated_ : virtual public ETCS::Entity
{
public:
    virtual ~Animated_() = default;

    // Is there anything left to advance? Asked before every step, and the
    // answer is allowed to change on its own (a fade that has arrived, a
    // held button that was let go).
    virtual bool Animating() = 0;

    // One step. The base measures how long it has been since the last one and
    // hands that to the leaf, so what a leaf writes is a rate rather than a
    // per-visit increment.
    virtual void Advance() = 0;
};

#endif // SUPERTYPE_ANIMATED_H__
