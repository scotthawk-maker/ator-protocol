/* Copyright (c) 2008-2009, The Tor Project, Inc. */
/* See LICENSE for licensing information */

/** \file memarea.c
 * \brief Implementation for memarea_t, an allocator for allocating lots of
 * small objects that will be freed all at once.
 */

#include "orconfig.h"
#include <stdlib.h>
#include "memarea.h"
#include "util.h"
#include "compat.h"
#include "log.h"

/** All returned pointers should be aligned to the nearest multiple of this
 * value. */
#define MEMAREA_ALIGN SIZEOF_VOID_P

#if MEMAREA_ALIGN == 4
#define MEMAREA_ALIGN_MASK 3lu
#elif MEMAREA_ALIGN == 8
#define MEMAREA_ALIGN_MASK 7lu
#else
#error "void* is neither 4 nor 8 bytes long. I don't know how to align stuff."
#endif

/** Increment <b>ptr</b> until it is aligned to MEMAREA_ALIGN. */
static INLINE void *
realign_pointer(void *ptr)
{
  uintptr_t x = (uintptr_t)ptr;
  x = (x+MEMAREA_ALIGN_MASK) & ~MEMAREA_ALIGN_MASK;
  tor_assert(((void*)x) >= ptr); // XXXX021 remove this once bug 930 is solved
  return (void*)x;
}

/** Implements part of a memarea.  New memory is carved off from chunk->mem in
 * increasing order until a request is too big, at which point a new chunk is
 * allocated. */
typedef struct memarea_chunk_t {
  /** Next chunk in this area. Only kept around so we can free it. */
  struct memarea_chunk_t *next_chunk;
  size_t mem_size; /**< How much RAM is available in u.mem, total? */
  char *next_mem; /**< Next position in u.mem to allocate data at.  If it's
                   * greater than or equal to mem+mem_size, this chunk is
                   * full. */
  union {
    char mem[1]; /**< Memory space in this chunk.  */
    void *_void_for_alignment; /**< Dummy; used to make sure mem is aligned. */
  } u;
} memarea_chunk_t;

#define CHUNK_HEADER_SIZE STRUCT_OFFSET(memarea_chunk_t, u)

#define CHUNK_SIZE 4096

/** A memarea_t is an allocation region for a set of small memory requests
 * that will all be freed at once. */
struct memarea_t {
  memarea_chunk_t *first; /**< Top of the chunk stack: never NULL. */
};

/** How many chunks will we put into the freelist before freeing them? */
#define MAX_FREELIST_LEN 4
/** The number of memarea chunks currently in our freelist. */
static int freelist_len=0;
/** A linked list of unused memory area chunks.  Used to prevent us from
 * spinning in malloc/free loops. */
static memarea_chunk_t *freelist = NULL;

/** Helper: allocate a new memarea chunk of around <b>chunk_size</b> bytes. */
static memarea_chunk_t *
alloc_chunk(size_t sz, int freelist_ok)
{
  if (freelist && freelist_ok) {
    memarea_chunk_t *res = freelist;
    freelist = res->next_chunk;
    res->next_chunk = NULL;
    --freelist_len;
    return res;
  } else {
    size_t chunk_size = freelist_ok ? CHUNK_SIZE : sz;
    memarea_chunk_t *res = tor_malloc_roundup(&chunk_size);
    res->next_chunk = NULL;
    res->mem_size = chunk_size - CHUNK_HEADER_SIZE;
    res->next_mem = res->u.mem;
    tor_assert(res->next_mem+res->mem_size == ((char*)res)+chunk_size);
    tor_assert(realign_pointer(res->next_mem) == res->next_mem);
    return res;
  }
}

/** Release <b>chunk</b> from a memarea, either by adding it to the freelist
 * or by freeing it if the freelist is already too big. */
static void
chunk_free(memarea_chunk_t *chunk)
{
  if (freelist_len < MAX_FREELIST_LEN) {
    ++freelist_len;
    chunk->next_chunk = freelist;
    freelist = chunk;
    chunk->next_mem = chunk->u.mem;
  } else {
    tor_free(chunk);
  }
}

/** Allocate and return new memarea. */
memarea_t *
memarea_new(void)
{
  memarea_t *head = tor_malloc(sizeof(memarea_t));
  head->first = alloc_chunk(CHUNK_SIZE, 1);
  return head;
}

/** Free <b>area</b>, invalidating all pointers returned from memarea_alloc()
 * and friends for this area */
void
memarea_drop_all(memarea_t *area)
{
  memarea_chunk_t *chunk, *next;
  for (chunk = area->first; chunk; chunk = next) {
    next = chunk->next_chunk;
    chunk_free(chunk);
  }
  area->first = NULL; /*fail fast on */
  tor_free(area);
}

/** Forget about having allocated anything in <b>area</b>, and free some of
 * the backing storage associated with it, as appropriate. Invalidates all
 * pointers returned from memarea_alloc() for this area. */
void
memarea_clear(memarea_t *area)
{
  memarea_chunk_t *chunk, *next;
  if (area->first->next_chunk) {
    for (chunk = area->first->next_chunk; chunk; chunk = next) {
      next = chunk->next_chunk;
      chunk_free(chunk);
    }
    area->first->next_chunk = NULL;
  }
  area->first->next_mem = area->first->u.mem;
}

/** Remove all unused memarea chunks from the internal freelist. */
void
memarea_clear_freelist(void)
{
  memarea_chunk_t *chunk, *next;
  freelist_len = 0;
  for (chunk = freelist; chunk; chunk = next) {
    next = chunk->next_chunk;
    tor_free(chunk);
  }
  freelist = NULL;
}

/** Return true iff <b>p</b> is in a range that has been returned by an
 * allocation from <b>area</b>. */
int
memarea_owns_ptr(const memarea_t *area, const void *p)
{
  memarea_chunk_t *chunk;
  const char *ptr = p;
  for (chunk = area->first; chunk; chunk = chunk->next_chunk) {
    if (ptr >= chunk->u.mem && ptr < chunk->next_mem)
      return 1;
  }
  return 0;
}

/** Return a pointer to a chunk of memory in <b>area</b> of at least <b>sz</b>
 * bytes.  <b>sz</b> should be significantly smaller than the area's chunk
 * size, though we can deal if it isn't. */
void *
memarea_alloc(memarea_t *area, size_t sz)
{
  memarea_chunk_t *chunk = area->first;
  char *result;
  tor_assert(chunk);
  if (sz == 0)
    sz = 1;
  if (chunk->next_mem+sz > chunk->u.mem+chunk->mem_size) {
    if (sz+CHUNK_HEADER_SIZE >= CHUNK_SIZE) {
      /* This allocation is too big.  Stick it in a special chunk, and put
       * that chunk second in the list. */
      memarea_chunk_t *new_chunk = alloc_chunk(sz+CHUNK_HEADER_SIZE, 0);
      new_chunk->next_chunk = chunk->next_chunk;
      chunk->next_chunk = new_chunk;
      chunk = new_chunk;
    } else {
      memarea_chunk_t *new_chunk = alloc_chunk(CHUNK_SIZE, 1);
      new_chunk->next_chunk = chunk;
      area->first = chunk = new_chunk;
    }
    tor_assert(chunk->mem_size >= sz);
  }
  result = chunk->next_mem;
  chunk->next_mem = chunk->next_mem + sz;
  // XXXX021 remove these once bug 930 is solved.
  tor_assert(chunk->next_mem >= chunk->u.mem);
  tor_assert(chunk->next_mem <= chunk->u.mem+chunk->mem_size);
  chunk->next_mem = realign_pointer(chunk->next_mem);
  return result;
}

/** As memarea_alloc(), but clears the memory it returns. */
void *
memarea_alloc_zero(memarea_t *area, size_t sz)
{
  void *result = memarea_alloc(area, sz);
  memset(result, 0, sz);
  return result;
}

/** As memdup, but returns the memory from <b>area</b>. */
void *
memarea_memdup(memarea_t *area, const void *s, size_t n)
{
  char *result = memarea_alloc(area, n);
  memcpy(result, s, n);
  return result;
}

/** As strdup, but returns the memory from <b>area</b>. */
char *
memarea_strdup(memarea_t *area, const char *s)
{
  return memarea_memdup(area, s, strlen(s)+1);
}

/** As strndup, but returns the memory from <b>area</b>. */
char *
memarea_strndup(memarea_t *area, const char *s, size_t n)
{
  size_t ln;
  char *result;
  const char *cp, *end = s+n;
  for (cp = s; cp < end && *cp; ++cp)
    ;
  /* cp now points to s+n, or to the 0 in the string. */
  ln = cp-s;
  result = memarea_alloc(area, ln+1);
  memcpy(result, s, ln);
  result[ln]='\0';
  return result;
}

/** Set <b>allocated_out</b> to the number of bytes allocated in <b>area</b>,
 * and <b>used_out</b> to the number of bytes currently used. */
void
memarea_get_stats(memarea_t *area, size_t *allocated_out, size_t *used_out)
{
  size_t a = 0, u = 0;
  memarea_chunk_t *chunk;
  for (chunk = area->first; chunk; chunk = chunk->next_chunk) {
    a += CHUNK_HEADER_SIZE + chunk->mem_size;
    tor_assert(chunk->next_mem >= chunk->u.mem);
    u += CHUNK_HEADER_SIZE + (chunk->next_mem - chunk->u.mem);
  }
  *allocated_out = a;
  *used_out = u;
}

/** Assert that <b>area</b> is okay. */
void
memarea_assert_ok(memarea_t *area)
{
  memarea_chunk_t *chunk;
  tor_assert(area->first);

  for (chunk = area->first; chunk; chunk = chunk->next_chunk) {
    tor_assert(chunk->next_mem >= chunk->u.mem);
    tor_assert(chunk->next_mem <=
          (char*) realign_pointer(chunk->u.mem+chunk->mem_size));
  }
}

