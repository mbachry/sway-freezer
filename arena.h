// Copyright 2022 Alexey Kutepov <reximkut@gmail.com>

// Permission is hereby granted, free of charge, to any person obtaining
// a copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to
// the following conditions:

// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
// LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
// OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#pragma once

#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Region Region;

struct Region {
    Region *next;
    size_t count;
    size_t capacity;
    uintptr_t data[];
};

typedef struct {
    Region *begin, *end;
} Arena;

typedef struct {
    Region *region;
    size_t count;
} Arena_Mark;

#define REGION_DEFAULT_CAPACITY (8 * 1024)

/* Region *new_region(size_t capacity); */
/* void free_region(Region *r); */

/* void *arena_alloc(Arena *a, size_t size_bytes); */
/* void *arena_realloc(Arena *a, void *oldptr, size_t oldsz, size_t newsz); */
/* char *arena_strdup(Arena *a, const char *cstr); */
/* void *arena_memdup(Arena *a, void *data, size_t size); */
/* char *arena_sprintf(Arena *a, const char *format, ...); */

/* Arena_Mark arena_snapshot(Arena *a); */
/* void arena_reset(Arena *a); */
/* void arena_rewind(Arena *a, Arena_Mark m); */
/* void arena_free(Arena *a); */
/* void arena_trim(Arena *a); */

#define ARENA_DA_INIT_CAP 256

#ifdef __cplusplus
#define cast_ptr(ptr) (decltype(ptr))
#else
#define cast_ptr(...)
#endif

#define arena_da_append(a, da, item)                                                                                   \
    do {                                                                                                               \
        if ((da)->count >= (da)->capacity) {                                                                           \
            size_t new_capacity = (da)->capacity == 0 ? ARENA_DA_INIT_CAP : (da)->capacity * 2;                        \
            (da)->items = cast_ptr((da)->items) arena_realloc((a), (da)->items, (da)->capacity * sizeof(*(da)->items), \
                                                              new_capacity * sizeof(*(da)->items));                    \
            (da)->capacity = new_capacity;                                                                             \
        }                                                                                                              \
                                                                                                                       \
        (da)->items[(da)->count++] = (item);                                                                           \
    } while (0)

// TODO: instead of accepting specific capacity new_region() should accept the size of the object we want to fit into
// the region It should be up to new_region() to decide the actual capacity to allocate
Region *new_region(size_t capacity)
{
    size_t size_bytes = sizeof(Region) + sizeof(uintptr_t) * capacity;
    // TODO: it would be nice if we could guarantee that the regions are allocated by ARENA_BACKEND_LIBC_MALLOC are page
    // aligned
    Region *r = (Region *)malloc(size_bytes);
    assert(r);
    r->next = NULL;
    r->count = 0;
    r->capacity = capacity;
    return r;
}

void free_region(Region *r) { free(r); }

// TODO: add debug statistic collection mode for arena
// Should collect things like:
// - How many times new_region was called
// - How many times existing region was skipped
// - How many times allocation exceeded REGION_DEFAULT_CAPACITY

void *arena_alloc(Arena *a, size_t size_bytes)
{
    size_t size = (size_bytes + sizeof(uintptr_t) - 1) / sizeof(uintptr_t);

    if (a->end == NULL) {
        assert(a->begin == NULL);
        size_t capacity = REGION_DEFAULT_CAPACITY;
        if (capacity < size)
            capacity = size;
        a->end = new_region(capacity);
        a->begin = a->end;
    }

    while (a->end->count + size > a->end->capacity && a->end->next != NULL) {
        a->end = a->end->next;
    }

    if (a->end->count + size > a->end->capacity) {
        assert(a->end->next == NULL);
        size_t capacity = REGION_DEFAULT_CAPACITY;
        if (capacity < size)
            capacity = size;
        a->end->next = new_region(capacity);
        a->end = a->end->next;
    }

    void *result = &a->end->data[a->end->count];
    a->end->count += size;
    return result;
}

void *arena_realloc(Arena *a, void *oldptr, size_t oldsz, size_t newsz)
{
    if (newsz <= oldsz)
        return oldptr;
    void *newptr = arena_alloc(a, newsz);
    char *newptr_char = (char *)newptr;
    char *oldptr_char = (char *)oldptr;
    for (size_t i = 0; i < oldsz; ++i) {
        newptr_char[i] = oldptr_char[i];
    }
    return newptr;
}

char *arena_strdup(Arena *a, const char *cstr)
{
    size_t n = strlen(cstr);
    char *dup = (char *)arena_alloc(a, n + 1);
    memcpy(dup, cstr, n);
    dup[n] = '\0';
    return dup;
}

void *arena_memdup(Arena *a, void *data, size_t size) { return memcpy(arena_alloc(a, size), data, size); }

char *arena_sprintf(Arena *a, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int n = vsnprintf(NULL, 0, format, args);
    va_end(args);

    assert(n >= 0);
    char *result = (char *)arena_alloc(a, n + 1);
    va_start(args, format);
    vsnprintf(result, n + 1, format, args);
    va_end(args);

    return result;
}

Arena_Mark arena_snapshot(Arena *a)
{
    Arena_Mark m;
    if (a->end == NULL) { // snapshot of uninitialized arena
        assert(a->begin == NULL);
        m.region = a->end;
        m.count = 0;
    } else {
        m.region = a->end;
        m.count = a->end->count;
    }

    return m;
}

void arena_reset(Arena *a)
{
    for (Region *r = a->begin; r != NULL; r = r->next) {
        r->count = 0;
    }

    a->end = a->begin;
}

void arena_rewind(Arena *a, Arena_Mark m)
{
    if (m.region == NULL) { // snapshot of uninitialized arena
        arena_reset(a);     // leave allocation
        return;
    }

    m.region->count = m.count;
    for (Region *r = m.region->next; r != NULL; r = r->next) {
        r->count = 0;
    }

    a->end = m.region;
}

void arena_free(Arena *a)
{
    Region *r = a->begin;
    while (r) {
        Region *r0 = r;
        r = r->next;
        free_region(r0);
    }
    a->begin = NULL;
    a->end = NULL;
}

void arena_trim(Arena *a)
{
    Region *r = a->end->next;
    while (r) {
        Region *r0 = r;
        r = r->next;
        free_region(r0);
    }
    a->end->next = NULL;
}
