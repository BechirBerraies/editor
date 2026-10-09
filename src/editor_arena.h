// editor_arena.h - virtual arenas: a reserved range that commits pages as it grows.
// Addresses never move, so a structure can grow in place.
#ifndef EDITOR_ARENA_H
#define EDITOR_ARENA_H

#include "editor.h"

#define ARENA_COMMIT_GRANULARITY KILOBYTES(64)

typedef struct Arena {
    u8 *base;
    u64 reserved;
    u64 committed;
    u64 used;
    b32 (*commit)(void *base, u64 size);
} Arena;

static Arena arena_make(u8 *base, u64 reserved, b32 (*commit)(void *base, u64 size))
{
    Arena arena = {0};
    arena.base = base;
    arena.reserved = reserved;
    arena.commit = commit;
    return arena;
}

static b32 arena_ensure_committed(Arena *arena, u64 size)
{
    if (size <= arena->committed) return 1;
    if (size > arena->reserved) return 0;

    u64 target = (size + ARENA_COMMIT_GRANULARITY - 1) & ~(ARENA_COMMIT_GRANULARITY - 1);
    if (target > arena->reserved) target = arena->reserved;
    if (!arena->commit(arena->base + arena->committed, target - arena->committed)) return 0;
    arena->committed = target;
    return 1;
}

// Returns 16-byte aligned memory, not zeroed. NULL when the arena is full.
static void *arena_push(Arena *arena, u64 size)
{
    u64 start = (arena->used + 15) & ~(u64)15;
    if (start > arena->reserved || size > arena->reserved - start) return 0;
    if (!arena_ensure_committed(arena, start + size)) return 0;
    arena->used = start + size;
    return arena->base + start;
}

// Committed pages are kept for reuse.
static void arena_reset(Arena *arena)
{
    arena->used = 0;
}

#endif
