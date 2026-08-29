#ifndef ARENA_ALLOCATOR_H__
#define ARENA_ALLOCATOR_H__
#include "MemoryArena.h"
namespace ETCS
{
template<typename T>
struct ArenaAllocator
{
    using value_type = T;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using propagate_on_container_copy_assignment = std::true_type;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;
    MemoryArena* arena;
    
    ArenaAllocator() noexcept : arena(nullptr) {}
    explicit ArenaAllocator(MemoryArena* a) noexcept : arena(a) {}
    // Rebind copy constructor — critical for unordered_map internal node allocs
    template<typename U>
    ArenaAllocator(const ArenaAllocator<U>& other) noexcept : arena(other.arena) {}

    // Checks the arena's own free list first (MemoryArena::
    // tryAcquireFromFreeList) before ever bump-allocating fresh space --
    // see that method's own comment (MemoryArena.h) for the full
    // reasoning. This is what makes EVERY ArenaAllocator-backed
    // container's node churn individually reclaim-capable, not just
    // Entity outer shells: a std::unordered_map (or any other STL
    // container) built on this allocator already calls deallocate()
    // faithfully on every node erase -- that call was previously a
    // no-op (see deallocate's own comment below for why that was
    // structurally sound but incomplete), so nothing ever came back
    // until the whole arena tore down. Keyed by (n * sizeof(T),
    // alignof(T)) -- for node allocations n is almost always 1, but a
    // container's own bucket-array reallocation (a single call with n
    // == the new bucket count) is keyed by ITS OWN size too, so it
    // only reuses a previously-freed block of the identical bucket
    // count -- correct either way, just less effective for that rarer,
    // more size-varying case than for the much more common
    // fixed-size-node case.
    T* allocate(std::size_t n)
    {
        if (!arena) throw std::bad_alloc();
        long long size  = static_cast<long long>(n * sizeof(T));
        long long align = static_cast<long long>(alignof(T));
        void* ptr = arena->tryAcquireFromFreeList(size, align);
        if (!ptr)
            ptr = arena->allocateRaw(size, align);
        if (!ptr) throw std::bad_alloc();
        return static_cast<T*>(ptr);
    }

    // Pushes back to the arena's own free list (MemoryArena::
    // releaseToFreeList) instead of discarding the pointer -- this used
    // to be a genuine, deliberate no-op ("arena reclaims all memory at
    // once"), which was correct for the arena's OWN bulk teardown but
    // left every individual node erase (RIDList<T>::remove, Entity's own
    // tags/flags_/typed_children_ erasing an entry, anything else built
    // on this allocator) with nowhere for its own bytes to go until the
    // whole arena died -- a real, confirmed leak under sustained churn
    // of any ArenaAllocator-backed container, not something specific to
    // one type. Safe regardless of what T is: by the time any conforming
    // allocator's own deallocate() runs, the container has already
    // destructed whatever object lived here (allocator_traits' own
    // contract -- destroy() always precedes deallocate()), so
    // releaseToFreeList's own zeroing is writing into memory whose C++
    // object lifetime has already ended, never into a live object.
    void deallocate(T* ptr, std::size_t n) noexcept
    {
        if (!arena || !ptr) return;
        arena->releaseToFreeList(ptr,
            static_cast<long long>(n * sizeof(T)),
            static_cast<long long>(alignof(T)));
    }
    template<typename U>
    bool operator==(const ArenaAllocator<U>& other) const noexcept { return arena == other.arena; }
    template<typename U>
    bool operator!=(const ArenaAllocator<U>& other) const noexcept { return arena != other.arena; }
};
} // namespace ETCS
#endif
