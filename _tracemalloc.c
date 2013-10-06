/* The implementation of the hash table (hash_t) is based on the cfuhash
   project:
   http://sourceforge.net/projects/libcfu/

   Copyright of cfuhash:
   ----------------------------------
   Creation date: 2005-06-24 21:22:40
   Authors: Don
   Change log:

   Copyright (c) 2005 Don Owens
   All rights reserved.

   This code is released under the BSD license:

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

     * Redistributions of source code must retain the above copyright
       notice, this list of conditions and the following disclaimer.

     * Redistributions in binary form must reproduce the above
       copyright notice, this list of conditions and the following
       disclaimer in the documentation and/or other materials provided
       with the distribution.

     * Neither the name of the author nor the names of its
       contributors may be used to endorse or promote products derived
       from this software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
   FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
   COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
   INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
   (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
   SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
   HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
   STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
   ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
   OF THE POSSIBILITY OF SUCH DAMAGE.
   ----------------------------------
*/

#define VERSION "1.0dev"

#include "Python.h"
#include "frameobject.h"
#include "pythread.h"
#include "osdefs.h"
#ifdef NT_THREADS
#  include <Windows.h>
#endif
#ifdef MS_WINDOWS
#  include <Psapi.h>
#endif

#if PY_MAJOR_VERSION >= 3
# define PYTHON3
#endif

#if defined(PYTHON3) || (PY_MAJOR_VERSION >= 2 && PY_MINOR_VERSION >= 7)
#  define PYINT_FROM_SSIZE_T PyLong_FromSize_t
#else
#  define PYINT_FROM_SSIZE_T PyInt_FromSize_t
#endif

#if PY_MAJOR_VERSION >= 3 && PY_MINOR_VERSION >= 3
#  define PEP393
#  define STRING_READ(KIND, DATA, INDEX) PyUnicode_READ(KIND, DATA, INDEX)
#  define STRING_WRITE(KIND, DATA, INDEX, CHAR) PyUnicode_WRITE(KIND, DATA, INDEX, CHAR)
#  define STRING_LENGTH(STR) PyUnicode_GET_LENGTH(STR)
#  define STRING_DATA(STR) PyUnicode_DATA(STR)

#elif defined(PYTHON3)
#  define STRING_READ(KIND, DATA, INDEX) ((DATA)[INDEX])
#  define STRING_WRITE(KIND, DATA, INDEX, CHAR) \
    do { STRING_READ(KIND, DATA, INDEX) = (CHAR); } while (0)
#  define STRING_LENGTH(STR) PyUnicode_GET_LENGTH(STR)
#  define STRING_DATA(STR) PyUnicode_AS_UNICODE(STR)

#else
#  define STRING_READ(KIND, DATA, INDEX) ((DATA)[INDEX])
#  define STRING_WRITE(KIND, DATA, INDEX, CHAR) \
    do { STRING_READ(KIND, DATA, INDEX) = (CHAR); } while (0)
#  define STRING_LENGTH(STR) PyString_GET_SIZE(STR)
#  define STRING_DATA(STR) PyString_AS_STRING(STR)
#endif

#ifdef PYTHON3
#  define STRING_COMPARE(str1, str2) PyUnicode_Compare(str1, str2)
#  define STRING_CHECK(obj) PyUnicode_Check(obj)
#  define CHAR_LOWER _PyUnicode_ToLowercase
#else
#  define STRING_COMPARE(str1, str2) PyObject_Compare(str1, str2)
#  define STRING_CHECK(obj) PyString_Check(obj)
#  define CHAR_LOWER tolower
#endif


#ifndef PYTHON3
typedef long Py_hash_t;
typedef unsigned long Py_uhash_t;
#endif

#ifndef _PyHASH_MULTIPLIER
#define _PyHASH_MULTIPLIER 1000003UL  /* 0xf4243 */
#endif

#ifndef Py_TYPE
   /* Python 2.5 doesn't have this macro */
#  define Py_TYPE(obj) obj->ob_type
#endif

/* Page of a memory page */
#define PAGE_SIZE 4096
#define PAGE_SIZE_BITS 12

/* Trace also memory blocks allocated by PyMem_RawMalloc() */
#define TRACE_RAW_MALLOC

/* Use a memory pool to release the memory as soon as traces are deleted,
   and to reduce the fragmentation of the heap */
#define USE_MEMORY_POOL

#if defined(HAVE_PTHREAD_ATFORK) && defined(WITH_THREAD)
#  define TRACE_ATFORK

/* Forward declaration */
static void tracemalloc_atfork(void);
#endif

/* Forward declaration */
static int tracemalloc_disable(void);
static void* raw_malloc(size_t size);
static void* raw_realloc(void *ptr, size_t size);
static void raw_free(void *ptr);
static void task_list_check(void);
static int task_list_clear(void);
#ifdef USE_MEMORY_POOL
static void* raw_alloc_arena(size_t size);
static void raw_free_arena(void *ptr, size_t size);
#endif

#ifdef MS_WINDOWS
#  define TRACE_CASE_INSENSITIVE
#endif

#if defined(ALTSEP) || defined(TRACE_CASE_INSENSITIVE)
#  define TRACE_NORMALIZE_FILENAME
#endif

#ifdef TRACE_ATFORK
#  include <pthread.h>
#endif

#ifdef Py_DEBUG
#  define TRACE_DEBUG
#endif

#if !defined(PRINT_STATS) && (defined(DEBUG_POOL) || defined(DEBUG_HASH_TABLE))
#  define PRINT_STATS
#endif

#define INT_TO_POINTER(value) ((void*)(Py_uintptr_t)(int)(value))
#define POINTER_TO_INT(ptr) ((int)(Py_uintptr_t)(ptr))

/* number chosen to limit the number of compaction of a memory pool */
#define MAX_ARENAS 32

/* Thresholds for the defragmentation of memory pool.
   Factor chosen as a cpu/memory compromise */
#define MAX_FRAG 0.50

/* Value chosen to limit the number of memory mappings per process
   and not waste too much memory */
#define ARENA_MIN_SIZE (128 * 1024)

#define HASH_MIN_SIZE 32
#define HASH_HIGH 0.50
#define HASH_LOW 0.10
#define HASH_REHASH_FACTOR 2.0 / (HASH_LOW + HASH_HIGH)

typedef struct slist_item_s {
    struct slist_item_s *next;
} slist_item_t;

#define SLIST_ITEM_NEXT(ITEM) (((slist_item_t *)ITEM)->next)

typedef struct {
    slist_item_t *head;
} slist_t;

#define SLIST_HEAD(SLIST) (((slist_t *)SLIST)->head)

typedef struct {
    /* used by hash_t.buckets to link entries */
    slist_item_t slist_item;

    const void *key;
    Py_uhash_t key_hash;

    /* data folllows */
} hash_entry_t;

#define BUCKETS_HEAD(SLIST) ((hash_entry_t *)SLIST_HEAD(&(SLIST)))
#define TABLE_HEAD(HT, BUCKET) ((hash_entry_t *)SLIST_HEAD(&(HT)->buckets[BUCKET]))
#define ENTRY_NEXT(ENTRY) ((hash_entry_t *)SLIST_ITEM_NEXT(ENTRY))

#define ENTRY_DATA_PTR(ENTRY) \
        ((char *)(ENTRY) + sizeof(hash_entry_t))
#define HASH_ENTRY_DATA_AS_VOID_P(ENTRY) \
        (*(void **)ENTRY_DATA_PTR(ENTRY))

#define HASH_ENTRY_READ_DATA(TABLE, DATA, DATA_SIZE, ENTRY) \
    do { \
        assert((DATA_SIZE) == (TABLE)->data_size); \
        memcpy(DATA, ENTRY_DATA_PTR(ENTRY), DATA_SIZE); \
    } while (0)

#define HASH_ENTRY_WRITE_DATA(TABLE, ENTRY, DATA, DATA_SIZE) \
    do { \
        assert((DATA_SIZE) == (TABLE)->data_size); \
        memcpy(ENTRY_DATA_PTR(ENTRY), DATA, DATA_SIZE); \
    } while (0)

#define HASH_GET_DATA(TABLE, KEY, DATA) \
    hash_get_data(TABLE, KEY, &(DATA), sizeof(DATA))

#define HASH_PUT_DATA(TABLE, KEY, DATA) \
    hash_put_data(TABLE, KEY, &(DATA), sizeof(DATA))

typedef struct {
    size_t length;
    size_t used;
    slist_t free_list;
    char *items;
} arena_t;

typedef struct {
    size_t item_size;
    size_t used;
    size_t length;
    unsigned int narena;
    arena_t arenas[MAX_ARENAS];
    slist_t hash_tables;
#ifdef PRINT_STATS
    const char *name;
    int debug;
#endif
} pool_t;

typedef struct {
    size_t data;
    size_t free;
} mem_stats_t;

typedef Py_uhash_t (*key_hash_func) (const void *key);
typedef int (*key_compare_func) (const void *key, hash_entry_t *he);
typedef void* (*hash_copy_data_func)(void *data);
typedef void (*hash_free_data_func)(void *data);
typedef void (*hash_get_data_size_func)(void *data, mem_stats_t *stats);

typedef struct {
    /* used by pool_t.hash_tables to link tables */
    slist_item_t slist_item;

    size_t num_buckets;
    size_t entries; /* Total number of entries in the table. */
    slist_t *buckets;
    size_t data_size;

    pool_t *pool;

    key_hash_func hash_func;
    key_compare_func compare_func;
    hash_copy_data_func copy_data_func;
    hash_free_data_func free_data_func;
    hash_get_data_size_func get_data_size_func;
#ifdef PRINT_STATS
    const char *name;
    int debug;
#endif
} hash_t;

/* Forward declaration */
static void hash_rehash(hash_t *ht);
#ifdef USE_MEMORY_POOL
static int pool_defrag(pool_t *pool, size_t length);
#endif
#ifdef PRINT_STATS
static void pool_print_stats(pool_t *pool);
static void hash_print_stats(hash_t *ht);
#endif

#ifdef TRACE_DEBUG
static void
tracemalloc_error(const char *format, ...)
{
    va_list ap;
    fprintf(stderr, "tracemalloc: ");
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    fflush(stderr);
}
#endif

static void
slist_init(slist_t *list)
{
    list->head = NULL;
}

static void
slist_prepend(slist_t *list, slist_item_t *item)
{
    item->next = list->head;
    list->head = item;
}

#ifdef USE_MEMORY_POOL
static slist_item_t *
slist_pop(slist_t *list)
{
    slist_item_t *item;
    item = list->head;
    assert(item != NULL);
    list->head = item->next;
    return item;
}
#endif

static void
_slist_remove(slist_t *list, slist_item_t *previous, slist_item_t *item)
{
    if (previous != NULL)
        previous->next = item->next;
    else
        list->head = item->next;
}

/* This function has a complexity of O(n): don't use it on large lists */
static void
slist_remove(slist_t *list, slist_item_t *item)
{
    slist_item_t *previous, *it;

    previous = NULL;
    for (it = list->head; it != NULL; it = it->next) {
        if (it == item)
            break;
        previous = it;
    }
    /* not found? */
    assert(it != NULL);
    _slist_remove(list, previous, item);
}

static Py_uhash_t
key_hash_int(const void *key)
{
    return (Py_uhash_t)POINTER_TO_INT(key);
}

static Py_uhash_t
key_hash_ptr(const void *key)
{
    return (Py_uhash_t)_Py_HashPointer((void *)key);
}

#ifdef TRACE_ARENA
static Py_uhash_t
key_hash_arena_ptr(const void *key)
{
    Py_hash_t x;
    size_t y = (size_t)key;
    /* bottom 12 bits are likely to be 0 (arena pointers aligned on a page
       size, usually 4096 bytes); rotate y by 12 to avoid excessive hash
       collisions for dicts and sets */
    y = (y >> PAGE_SIZE_BITS) | (y << (8 * SIZEOF_VOID_P - PAGE_SIZE_BITS));
    x = (Py_hash_t)y;
    if (x == -1)
        x = -2;
    return x;
}
#endif

static Py_uhash_t
filename_hash(PyObject *filename)
{
    if (filename != NULL)
        return PyObject_Hash(filename);
    else
        return 0;
}

static int
key_cmp_unicode(const void *key, hash_entry_t *he)
{
    if (key != NULL && he->key != NULL)
        return (STRING_COMPARE((PyObject *)key, (PyObject *)he->key) == 0);
    else
        return key == he->key;
}

static int
key_cmp_direct(const void *key, hash_entry_t *he)
{
    return he->key == key;
}

/* makes sure the real size of the buckets array is a power of 2 */
static size_t
round_size(size_t s)
{
    size_t i;
    if (s < HASH_MIN_SIZE)
        return HASH_MIN_SIZE;
    i = 1;
    while (i < s)
        i <<= 1;
    return i;
}

static void
pool_init(pool_t *pool, size_t item_size)
{
    assert(item_size >= sizeof(slist_item_t));
    pool->item_size = item_size;
    pool->narena = 0;
    pool->used = 0;
    pool->length = 0;
    slist_init(&pool->hash_tables);
#ifdef PRINT_STATS
    pool->name = NULL;
    pool->debug = 1;
#endif
}

#ifdef USE_MEMORY_POOL
static int
arena_init(pool_t *pool, arena_t *arena, size_t length)
{
    size_t pool_size;
    char *ptr, *end;
    slist_item_t *previous, *item;

    assert(pool->item_size < PY_SIZE_MAX / length);
    pool_size = pool->item_size * length;

    arena->length = length;
    arena->used = 0;
    arena->items = raw_alloc_arena(pool_size);
    if (arena->items == NULL)
        return -1;

    ptr = arena->items;
    item = (slist_item_t *)ptr;
    end = ptr + pool_size;

    arena->free_list.head = item;

    previous = item;
    ptr += pool->item_size;
    while (ptr < end) {
        item = (slist_item_t *)ptr;
        previous->next = item;

        previous = item;
        ptr += pool->item_size;
    }
    previous->next = NULL;

    return 0;
}

static void
arena_free(pool_t *pool, arena_t *arena)
{
    size_t pool_size = pool->item_size * arena->length;
    assert(arena->used == 0);
    raw_free_arena(arena->items, pool_size);
}

static void
pool_copy(pool_t *copy, pool_t *pool)
{
    pool_init(copy, pool->item_size);
#ifdef PRINT_STATS
    copy->name = pool->name;
    copy->debug = pool->debug;
#endif
}

static int
pool_alloc_arena(pool_t *pool, size_t arena_length)
{
    assert(pool->narena < MAX_ARENAS);

    if (arena_init(pool, &pool->arenas[pool->narena], arena_length) < 0)
        return -1;

    pool->length += arena_length;
    pool->narena++;
    return 0;
}

static hash_entry_t*
arena_alloc_entry(pool_t *pool, arena_t *arena)
{
    if (arena->free_list.head == NULL) {
        assert(arena->used == arena->length);
        return NULL;
    }

    arena->used++;
    pool->used++;
    return (hash_entry_t *)slist_pop(&arena->free_list);
}

static int
pool_alloc(pool_t *pool, size_t items)
{
    size_t free, arena_length, min_length, new_length;
    const double factor = 1.0 - (MAX_FRAG / 2.0);
    unsigned int narena;
    double new_frag;

    free = pool->length - pool->used;
    if (items <= free)
        return 0;

    if (pool->narena == MAX_ARENAS) {
        /* too much arenas: compact into one unique arena */
        return pool_defrag(pool, items);
    }

    arena_length = items - free;

    min_length = pool->used + items;
    min_length -= (size_t)(pool->length * factor);
    min_length = (size_t)((double)min_length / factor);
    if (arena_length < min_length)
        arena_length = min_length;

    assert(pool->narena < MAX_ARENAS);
    narena = (pool->narena + 1);

    min_length = ARENA_MIN_SIZE / pool->item_size;
    if (arena_length < min_length)
        arena_length = min_length;

    /* don't defrag small tables */
    if (pool->narena > 0) {
        new_length = pool->length + arena_length;
        if (new_length / narena > ARENA_MIN_SIZE / pool->item_size
            /* two checks to avoid an integer overflow */
            && (new_length * pool->item_size / narena) > ARENA_MIN_SIZE)
        {
            free = new_length - pool->used - items;
            new_frag = (double)free / new_length;
            if (new_frag > MAX_FRAG) {
                /* allocating a new arena would increase the fragmentation */
                return pool_defrag(pool, arena_length);
            }
        }
    }

    return pool_alloc_arena(pool, arena_length);
}

static hash_entry_t*
pool_alloc_entry(pool_t *pool)
{
    int i;
    hash_entry_t* entry;

    for (i=pool->narena-1; i >= 0; i--) {
        entry = arena_alloc_entry(pool, &pool->arenas[i]);
        if (entry != NULL)
            return entry;
    }
    return NULL;
}

static int
arena_free_entry(pool_t *pool, arena_t *arena, hash_entry_t *entry)
{
    size_t pool_size;
    char *start, *end;

    pool_size = pool->item_size * arena->length;
    start = arena->items;
    end = start + pool_size;

    if ((char*)entry < start) {
        return 0;
    }
    if ((char*)entry >= end)
        return 0;

    assert(arena->used != 0);
    arena->used--;
    pool->used--;
    slist_prepend(&arena->free_list, (slist_item_t *)entry);
    return 1;
}

static void
pool_remove_arena(pool_t *pool, unsigned int index)
{
    arena_t *arena;
    size_t narena;

    /* don't delete the last arena */
    if (pool->narena == 1)
        return;

    /* the arena should not be removed if all other arenas are full */
    if (pool->used == pool->length)
        return;

    assert(index < pool->narena);
    arena = &pool->arenas[index];
    assert(arena->used == 0);

    assert(pool->length >= arena->length);
    pool->length -= arena->length;

    arena_free(pool, arena);
    narena = pool->narena - index;
    if (narena != 1) {
        memmove(&pool->arenas[index],
                &pool->arenas[index + 1],
                (narena - 1) * sizeof(arena_t));
    }
    assert(pool->narena >= 2);
    pool->narena--;

#ifdef DEBUG_POOL
    if (pool->debug) {
        printf("Pool %p remove arena: [%u arenas] %zu/%zu, frag=%.0f%%\n",
               pool, pool->narena,
               pool->used, pool->length,
               (pool->length - pool->used) * 100.0 / pool->length);
    }
#endif
}

static void
pool_clear(pool_t *pool)
{
    unsigned int i;
    assert(pool->used == 0);
    if (pool->narena != 0) {
        for (i=0; i < pool->narena; i++)
            arena_free(pool, &pool->arenas[i]);
        pool->narena = 0;
        pool->length = 0;
    }
    else {
        assert(pool->narena == 0);
        assert(pool->length == 0);
    }
}
#endif

static hash_entry_t*
hash_alloc_entry(hash_t *ht)
{
#ifdef USE_MEMORY_POOL
    hash_entry_t* entry;
    pool_t *pool = ht->pool;

    entry = pool_alloc_entry(pool);
    if (entry != NULL)
        return entry;

    assert(pool->length == pool->used);

    if (pool_alloc(pool, 1) < 0)
        return NULL;

    entry = arena_alloc_entry(pool, &pool->arenas[pool->narena-1]);
    assert(entry != NULL);
#ifdef DEBUG_POOL
    printf("[Allocate 1 entry] ");
    pool_print_stats(pool);
#endif
    return entry;
#else
    return malloc(ht->pool->item_size);
#endif
}

static void
pool_free_entry(pool_t *pool, hash_entry_t *entry)
{
#ifdef USE_MEMORY_POOL
    unsigned int i;
    arena_t *arena;

    for (i=0; i < pool->narena; i++) {
        arena = &pool->arenas[i];
        if (!arena_free_entry(pool, arena, entry)) {
            /* entry is not part of the arena */
            continue;
        }
        if (arena->used == 0)
            pool_remove_arena(pool, i);
        return;
    }
    /* at least one arena should be non empty */
    assert(0);
#else
    free(entry);
#endif
}

static hash_t *
hash_new_full(pool_t *pool, size_t data_size, size_t size,
              key_hash_func hash_func,
              key_compare_func compare_func,
              hash_copy_data_func copy_data_func,
              hash_free_data_func free_data_func,
              hash_get_data_size_func get_data_size_func)
{
    hash_t *ht;
    size_t buckets_size;

    assert(pool->item_size == sizeof(hash_entry_t) + data_size);

    ht = (hash_t *)raw_malloc(sizeof(hash_t));
    if (ht == NULL)
        return ht;

    ht->num_buckets = round_size(size);
    ht->entries = 0;
    ht->data_size = data_size;
    ht->pool = pool;

    buckets_size = ht->num_buckets * sizeof(ht->buckets[0]);
    ht->buckets = raw_malloc(buckets_size);
    if (ht->buckets == NULL) {
        raw_free(ht);
        return NULL;
    }
    memset(ht->buckets, 0, buckets_size);

    ht->hash_func = hash_func;
    ht->compare_func = compare_func;
    ht->copy_data_func = copy_data_func;
    ht->free_data_func = free_data_func;
    ht->get_data_size_func = get_data_size_func;
#ifdef PRINT_STATS
    ht->name = NULL;
    ht->debug = 1;
#endif

    slist_prepend(&ht->pool->hash_tables, (slist_item_t *)ht);
    return ht;
}

static hash_t *
hash_new(pool_t *pool, size_t data_size,
         key_hash_func hash_func,
         key_compare_func compare_func)
{
    return hash_new_full(pool, data_size, HASH_MIN_SIZE,
                         hash_func, compare_func,
                         NULL, NULL, NULL);
}

static void
pool_mem_stats(pool_t *pool, mem_stats_t *stats)
{
    stats->data += sizeof(pool_t);
    stats->data += pool->narena * sizeof(arena_t);
    stats->data += pool->item_size * pool->used;
    stats->free += pool->item_size * (pool->length - pool->used);
}

/* Don't count memory allocated in the memory pool, only memory directly
   allocated by the hash table: see pool_mem_stats() */
static void
hash_mem_stats(hash_t *ht, mem_stats_t *stats)
{
    hash_entry_t *entry;
    size_t hv;

    stats->data += sizeof(hash_t);

    /* buckets */
    stats->data += ht->num_buckets * sizeof(hash_entry_t *);

#ifndef USE_MEMORY_POOL
    stats->data += ht->entries * sizeof(hash_entry_t);
    stats->data += ht->entries * ht->data_size;
#endif

    /* data linked from entries */
    if (ht->get_data_size_func) {
        for (hv = 0; hv < ht->num_buckets; hv++) {
            for (entry = TABLE_HEAD(ht, hv); entry; entry = ENTRY_NEXT(entry)) {
                void *data = HASH_ENTRY_DATA_AS_VOID_P(entry);
                ht->get_data_size_func(data, stats);
            }
        }
    }
}

#ifdef PRINT_STATS
static void
pool_print_stats(pool_t *pool)
{
    size_t size;
    mem_stats_t stats;
    double usage, fragmentation;

    memset(&stats, 0, sizeof(stats));
    pool_mem_stats(pool, &stats);
    if (pool->length != 0)
        usage = pool->used * 100.0 / pool->length;
    else
        usage = 0.0;
    size = stats.data + stats.free;
    if (size)
        fragmentation = stats.free * 100.0 / size;
    else
        fragmentation = 0.0;
    printf("pool %s (%p): %u arenas, "
           "%zu/%zu items (%.0f%%), "
           "%zu/%zu kB (frag=%.1f%%)\n",
           pool->name, pool, pool->narena,
           pool->used, pool->length, usage,
           stats.data / 1024,  size / 1024, fragmentation);
}

static void
hash_print_stats(hash_t *ht)
{
    mem_stats_t stats;
    size_t chain_len, max_chain_len, total_chain_len, nchains;
    hash_entry_t *entry;
    size_t hv;
    double load;

    memset(&stats, 0, sizeof(stats));
    hash_mem_stats(ht, &stats);

    load = (double)ht->entries / ht->num_buckets;

    max_chain_len = 0;
    total_chain_len = 0;
    nchains = 0;
    for (hv = 0; hv < ht->num_buckets; hv++) {
        entry = TABLE_HEAD(ht, hv);
        if (entry != NULL) {
            chain_len = 0;
            for (; entry; entry = ENTRY_NEXT(entry)) {
                chain_len++;
            }
            if (chain_len > max_chain_len)
                max_chain_len = chain_len;
            total_chain_len += chain_len;
            nchains++;
        }
    }
    printf("hash table %s (%p): entries=%zu/%zu (%.0f%%), ",
           ht->name, ht, ht->entries, ht->num_buckets, load * 100.0);
    if (nchains)
        printf("avg_chain_len=%.1f, ", (double)total_chain_len / nchains);
    printf("max_chain_len=%zu, "
           "%zu/%zu kB\n",
           max_chain_len,
           stats.data / 1024, (stats.data + stats.free) / 1024);
}
#endif

/*
 Returns one if the entry was found, zero otherwise.  If found, r is
 changed to point to the data in the entry.
*/
static hash_entry_t *
hash_get_entry(hash_t *ht, const void *key)
{
    Py_uhash_t key_hash;
    size_t index;
    hash_entry_t *entry;

    key_hash = ht->hash_func(key);
    index = key_hash & (ht->num_buckets - 1);

    for (entry = TABLE_HEAD(ht, index); entry != NULL; entry = ENTRY_NEXT(entry)) {
        if (entry->key_hash == key_hash && ht->compare_func(key, entry))
            break;
    }

    return entry;
}

static int
_hash_pop_entry(hash_t *ht, const void *key, void *data, size_t data_size)
{
    Py_uhash_t key_hash;
    size_t index;
    hash_entry_t *entry, *previous;

    key_hash = ht->hash_func(key);
    index = key_hash & (ht->num_buckets - 1);

    previous = NULL;
    for (entry = TABLE_HEAD(ht, index); entry != NULL; entry = ENTRY_NEXT(entry)) {
        if (entry->key_hash == key_hash && ht->compare_func(key, entry))
            break;
        previous = entry;
    }

    if (entry == NULL)
        return 0;

    _slist_remove(&ht->buckets[index], (slist_item_t*)previous, (slist_item_t*)entry);
    ht->entries--;

    if (data != NULL)
        HASH_ENTRY_READ_DATA(ht, data, data_size, entry);
    pool_free_entry(ht->pool, entry);

    if ((float)ht->entries / (float)ht->num_buckets < HASH_LOW)
        hash_rehash(ht);
#ifdef USE_MEMORY_POOL
    pool_defrag(ht->pool, 0);
#endif
    return 1;
}

/* Add a new entry to the hash. Return 0 on success, -1 on memory error. */
static int
hash_put_data(hash_t *ht, const void *key, void *data, size_t data_size)
{
    Py_uhash_t key_hash;
    size_t index;
    hash_entry_t *entry;

    assert(data != NULL || data_size == 0);
#ifndef NDEBUG
    entry = hash_get_entry(ht, key);
    assert(entry == NULL);
#endif

    key_hash = ht->hash_func(key);
    index = key_hash & (ht->num_buckets - 1);

    entry = hash_alloc_entry(ht);
    if (entry == NULL) {
#ifdef TRACE_DEBUG
        tracemalloc_error("memory allocation failed in hash_put_data()");
#endif
        return -1;
    }

    entry->key = (void *)key;
    entry->key_hash = key_hash;
    HASH_ENTRY_WRITE_DATA(ht, entry, data, data_size);

    slist_prepend(&ht->buckets[index], (slist_item_t*)entry);
    ht->entries++;

    if ((float)ht->entries / (float)ht->num_buckets > HASH_HIGH)
        hash_rehash(ht);
    return 0;
}

/* Get data from an entry. Copy entry data into data and return 1 if the entry
   exists, return 0 if the entry does not exist. */
static int
hash_get_data(hash_t *ht, const void *key, void *data, size_t data_size)
{
    hash_entry_t *entry;

    assert(data != NULL);

    entry = hash_get_entry(ht, key);
    if (entry == NULL)
        return 0;
    HASH_ENTRY_READ_DATA(ht, data, data_size, entry);
    return 1;
}

static int
hash_pop_data(hash_t *ht, const void *key, void *data, size_t data_size)
{
    assert(data != NULL);
    assert(ht->free_data_func == NULL);
    return _hash_pop_entry(ht, key, data, data_size);
}

/* Try to delete an entry. Return 1 if the entry is deleted, 0 if the
   entry was not found. */
static int
hash_may_delete_data(hash_t *ht, const void *key)
{
    return _hash_pop_entry(ht, key, NULL, 0);
}

static void
hash_delete_data(hash_t *ht, const void *key)
{
#ifndef NDEBUG
    int found = hash_may_delete_data(ht, key);
    assert(found);
#else
    (void)hash_may_delete_data(ht, key);
#endif
}

/* Prototype for a pointer to a function to be called foreach
   key/value pair in the hash by hash_foreach().  Iteration
   stops if a non-zero value is returned. */
static int
hash_foreach(hash_t *ht,
             int (*fe_fn) (hash_entry_t *entry, void *arg),
             void *arg)
{
    hash_entry_t *entry;
    size_t hv;

    for (hv = 0; hv < ht->num_buckets; hv++) {
        for (entry = TABLE_HEAD(ht, hv); entry; entry = ENTRY_NEXT(entry)) {
            int res = fe_fn(entry, arg);
            if (res)
                return res;
        }
    }
    return 0;
}

static void
hash_rehash(hash_t *ht)
{
    size_t buckets_size, new_size, bucket;
    slist_t *old_buckets = NULL;
    size_t old_num_buckets;

    new_size = round_size(ht->entries * HASH_REHASH_FACTOR);
    if (new_size == ht->num_buckets)
        return;

#ifdef DEBUG_HASH_TABLE
    if (ht->debug) {
        printf("[before rehash] ");
        hash_print_stats(ht);
    }
#endif

    old_num_buckets = ht->num_buckets;

    buckets_size = new_size * sizeof(ht->buckets[0]);
    old_buckets = ht->buckets;
    ht->buckets = raw_malloc(buckets_size);
    if (ht->buckets == NULL) {
        /* cancel rehash on memory allocation failure */
        ht->buckets = old_buckets ;
#ifdef TRACE_DEBUG
        tracemalloc_error("memory allocation failed in hash_rehash()");
#endif
        return;
    }
    memset(ht->buckets, 0, buckets_size);

    ht->num_buckets = new_size;

    for (bucket = 0; bucket < old_num_buckets; bucket++) {
        hash_entry_t *entry, *next;
        for (entry = BUCKETS_HEAD(old_buckets[bucket]); entry != NULL; entry = next) {
            size_t entry_index;

            assert(ht->hash_func(entry->key) == entry->key_hash);
            next = ENTRY_NEXT(entry);
            entry_index = entry->key_hash & (new_size - 1);

            slist_prepend(&ht->buckets[entry_index], (slist_item_t*)entry);
        }
    }

    raw_free(old_buckets);

#ifdef DEBUG_HASH_TABLE
    if (ht->debug) {
        printf("[after rehash] ");
        hash_print_stats(ht);
    }
#endif
}

#ifdef USE_MEMORY_POOL
static void
hash_move_pool(hash_t *ht, pool_t *old_pool, pool_t *new_pool)
{
    size_t i;

    for (i = 0; i < ht->num_buckets; i++) {
        hash_entry_t *entry, *next, *new_entry;

        entry = TABLE_HEAD(ht, i);
        slist_init(&ht->buckets[i]);
        for (; entry != NULL; entry = next) {
            next = ENTRY_NEXT(entry);

            new_entry = pool_alloc_entry(new_pool);
            assert(new_entry != NULL);

            memcpy(new_entry, entry, ht->pool->item_size);
            pool_free_entry(old_pool, entry);

            slist_prepend(&ht->buckets[i], (slist_item_t*)new_entry);
        }
    }
}

static int
pool_defrag(pool_t *pool, size_t length)
{
    pool_t new_pool;
    slist_item_t *it, *next;
    size_t prealloc;

    if (length == 0) {
        /* an item has been removed */
        size_t free;

        /* don't defrag small tables */
        if (pool->length / pool->narena <= ARENA_MIN_SIZE / pool->item_size
            /* two checks to avoid an integer overflow */
            && (pool->length  * pool->item_size / pool->narena) <= ARENA_MIN_SIZE)
            return 0;

        /* fragmentation lower than the threshold? */
        free = pool->length - pool->used;
        if ((double)free <= MAX_FRAG * pool->length)
            return 0;
    }

#ifdef DEBUG_POOL
    if (pool->debug) {
        printf("[before defrag, length=%zu] ", length);
        pool_print_stats(pool);
    }
#endif

    pool_copy(&new_pool, pool);
    prealloc = pool->used + length;
    if (pool_alloc(&new_pool, prealloc) < 0) {
#ifdef TRACE_DEBUG
        tracemalloc_error("memory allocation failed in pool_defrag()");
#endif
        return -1;
    }

    /* move entries of the tables to the new pool */
    for (it=pool->hash_tables.head; it != NULL; it = next) {
        next = it->next;
        hash_move_pool((hash_t *)it, pool, &new_pool);
        slist_prepend(&new_pool.hash_tables, it);
    }

    slist_init(&pool->hash_tables);
    pool_clear(pool);
    *pool = new_pool;

#ifdef DEBUG_POOL
    if (pool->debug) {
        printf("[after defrag, length=%zu] ", length);
        pool_print_stats(pool);
    }
#endif
    return 0;
}
#endif

static void
hash_clear(hash_t *ht)
{
    hash_entry_t *entry, *next;
    size_t i;

    for (i=0; i < ht->num_buckets; i++) {
        for (entry = TABLE_HEAD(ht, i); entry != NULL; entry = next) {
            next = ENTRY_NEXT(entry);
            if (ht->free_data_func)
                ht->free_data_func(HASH_ENTRY_DATA_AS_VOID_P(entry));
            pool_free_entry(ht->pool, entry);
        }
        slist_init(&ht->buckets[i]);
    }
    ht->entries = 0;
    hash_rehash(ht);
#ifdef USE_MEMORY_POOL
    pool_defrag(ht->pool, 0);
#endif
}

static void
hash_destroy(hash_t *ht)
{
    size_t i;

    for (i = 0; i < ht->num_buckets; i++) {
        slist_item_t *entry = ht->buckets[i].head;
        while (entry) {
            slist_item_t *entry_next = entry->next;
            if (ht->free_data_func)
                ht->free_data_func(HASH_ENTRY_DATA_AS_VOID_P(entry));
            pool_free_entry(ht->pool, (hash_entry_t *)entry);
            entry = entry_next;
        }
    }

    slist_remove(&ht->pool->hash_tables, (slist_item_t *)ht);

    raw_free(ht->buckets);
    raw_free(ht);
}

static hash_t *
hash_copy_with_pool(hash_t *src, pool_t *pool)
{
    hash_t *dst;
    hash_entry_t *entry;
    size_t bucket;
    int err;
    void *data, *new_data;

#ifdef USE_MEMORY_POOL
    if (pool_alloc(pool, src->entries) < 0)
        return NULL;
#endif

    dst = hash_new_full(pool, src->data_size, src->num_buckets,
                        src->hash_func, src->compare_func,
                        src->copy_data_func, src->free_data_func,
                        src->get_data_size_func);
    if (dst == NULL)
        return NULL;

    for (bucket=0; bucket < src->num_buckets; bucket++) {
        entry = TABLE_HEAD(src, bucket);
        for (; entry; entry = ENTRY_NEXT(entry)) {
            if (src->copy_data_func) {
                data = HASH_ENTRY_DATA_AS_VOID_P(entry);
                new_data = src->copy_data_func(data);
                if (new_data != NULL)
                    err = hash_put_data(dst, entry->key,
                                        &new_data, src->data_size);
                else
                    err = 1;
            }
            else {
                data = ENTRY_DATA_PTR(entry);
                err = hash_put_data(dst, entry->key, data, src->data_size);
            }
            if (err) {
                hash_destroy(dst);
                return NULL;
            }
        }
    }
#ifdef DEBUG_POOL
    printf("[copy hash] ");
    pool_print_stats(dst->pool);
#endif
    return dst;
}

/* Return a copy of the hash table */
static hash_t *
hash_copy(hash_t *src)
{
    return hash_copy_with_pool(src, src->pool);
}

/* Arbitrary limit of the number of frames in a traceback. The value was chosen
   to not allocate too much memory on the stack (see TRACEBACK_STACK_SIZE
   below). */
#define MAX_NFRAME 100

/* arbitrary limit of the depth of the recursive
   match_filename_joker() function to avoid a stack overflow */
#define MAX_NJOKER 100

/* Protected by the GIL */
static struct {
    PyMemAllocator mem;
    PyMemAllocator raw;
    PyMemAllocator obj;
} allocators;

#ifdef TRACE_ARENA
/* Protected by the GIL */
static PyObjectArenaAllocator arena_allocator;
#endif

static struct {
    /* tracemalloc_init() was already called?
       Variable protected by the GIL */
    int init;

    /* is the module enabled?
       Variable protected by the GIL */
    int enabled;

    /* limit of the number of frames in a traceback, 1 by default.
       Variable protected by the GIL. */
    int max_nframe;
} tracemalloc_config = {0, 0, 1};

#if defined(WITH_THREAD) && defined(TRACE_RAW_MALLOC)
#define REENTRANT_THREADLOCAL

#ifdef NT_THREADS
static DWORD tracemalloc_reentrant_key = TLS_OUT_OF_INDEXES;

static int
get_reentrant(void)
{
    LPVOID ptr;
    int value;

    ptr = TlsGetValue(tracemalloc_reentrant_key);
    if (ptr == NULL) {
        assert(GetLastError() == ERROR_SUCCESS);
        return 0;
    }

    value = POINTER_TO_INT(ptr);
    return (value == 1);
}

static void
set_reentrant_tls(int reentrant)
{
    LPVOID ptr;
#ifndef NDEBUG
    BOOL res;
#endif

    ptr = INT_TO_POINTER(reentrant);
#ifndef NDEBUG
    res = TlsSetValue(tracemalloc_reentrant_key, ptr);
    assert(res != 0);
#else
    (void)TlsSetValue(tracemalloc_reentrant_key, ptr);
#endif
}

#elif defined(_POSIX_THREADS)
static pthread_key_t tracemalloc_reentrant_key;

static int
get_reentrant(void)
{
    void *ptr;
    int value;
    ptr = pthread_getspecific(tracemalloc_reentrant_key);
    if (ptr == NULL)
        return 0;
    value = POINTER_TO_INT(ptr);
    return (value == 1);
}

static void
set_reentrant_tls(int reentrant)
{
#ifndef NDEBUG
    int res;
#endif
    void *ptr;

    ptr = INT_TO_POINTER(reentrant);
#ifndef NDEBUG
    res = pthread_setspecific(tracemalloc_reentrant_key, ptr);
    assert(res == 0);
#else
    (void)pthread_setspecific(tracemalloc_reentrant_key, ptr);
#endif
}
#else
#   error "unsupported thread model"
#endif

#else

/* WITH_THREAD not defined: Python compiled without threads,
   or TRACE_RAW_MALLOC not defined: variable protected by the GIL */
static int tracemalloc_reentrant = 0;

#define get_reentrant() tracemalloc_reentrant
#define set_reentrant_tls(value) do { tracemalloc_reentrant = value; } while (0)

#endif

static void
set_reentrant(int reentrant)
{
    assert(!reentrant || !get_reentrant());
    set_reentrant_tls(reentrant);
}

typedef struct {
    /* include (1) or exclude (0) matching frame? */
    int include;
    Py_hash_t pattern_hash;
    PyObject *pattern;
#ifndef TRACE_NORMALIZE_FILENAME
    int use_joker;
#endif
    /* ignore any line number if lineno < 1 */
    int lineno;
    /* use the whole traceback, or only the most recent frame? */
    int traceback;
} filter_t;

typedef struct {
    size_t nfilter;
    filter_t *filters;
} filter_list_t;

#if defined(TRACE_RAW_MALLOC) && defined(WITH_THREAD)
/* This lock is needed because tracemalloc_raw_free() is called without
   the GIL held. It cannot acquire the lock because it would introduce
   a deadlock in PyThreadState_DeleteCurrent(). */
static PyThread_type_lock tables_lock;
#  define TABLES_LOCK() PyThread_acquire_lock(tables_lock, 1)
#  define TABLES_UNLOCK() PyThread_release_lock(tables_lock)
#else
   /* variables are protected by the GIL */
#  define TABLES_LOCK()
#  define TABLES_UNLOCK()
#endif

#ifdef WITH_THREAD
#endif

/* Protected by the GIL */
static filter_list_t tracemalloc_include_filters;
static filter_list_t tracemalloc_exclude_filters;

#pragma pack(4)
typedef struct
#ifdef __GNUC__
__attribute__((packed))
#endif
{
    PyObject *filename;
    int lineno;
} frame_t;

typedef struct {
    Py_uhash_t hash;
    int nframe;
    frame_t frames[1];
} traceback_t;

static traceback_t tracemalloc_empty_traceback;

typedef struct {
    size_t size;
    traceback_t *traceback;
} trace_t;

#define TRACEBACK_SIZE(NFRAME) \
        (sizeof(traceback_t) + sizeof(frame_t) * (NFRAME - 1))
#define TRACEBACK_STACK_SIZE TRACEBACK_SIZE(MAX_NFRAME)

typedef struct {
    int enabled;
    int failed;

    Py_ssize_t ncall;

    int delay;
    time_t timeout;

    Py_ssize_t memory_threshold;
    size_t min_traced;
    size_t max_traced;

    PyObject *func;
    PyObject *func_args;
    PyObject *func_kwargs;
} task_t;

/* List of tasks (PyListObject*).
   Protected by the GIL */
static PyObject* tracemalloc_tasks;

typedef struct {
    size_t size;
    size_t count;
} trace_stats_t;

/* Size of Currently traced memory.
   Protected by TABLES_LOCK(). */
static size_t tracemalloc_traced_memory = 0;

/* Maximum size of traced memory.
   Protected by TABLES_LOCK(). */
static size_t tracemalloc_max_traced_memory = 0;

#ifdef TRACE_ARENA
/* Total size of the arenas memory.
   Protected by the GIL. */
static size_t tracemalloc_arena_size = 0;
#endif

static struct {
    /* tracemalloc_filenames, tracemalloc_tracebacks.
       Protected by the GIL. */
    pool_t no_data;

    /* tracemalloc_traces and line_hash of tracemalloc_file_stats.
       Protected by TABLES_LOCK(). */
    pool_t traces;

    /* tracemalloc_file_stats: "hash_t*" type.
       Protected by TABLES_LOCK(). */
    pool_t hash_tables;
} tracemalloc_pools;

/* Hash table used to intern filenames.
   Protected by the GIL */
static hash_t *tracemalloc_filenames = NULL;

/* Hash table used to intern tracebacks.
   Protected by the GIL */
static hash_t *tracemalloc_tracebacks = NULL;

/* Statistics on Python memory allocations per file and per line:
   {filename: PyObject* => {lineno: int => stat: trace_stats_t}.
   Protected by TABLES_LOCK(). */
static hash_t *tracemalloc_file_stats = NULL;

/* pointer (void*) => trace (trace_t).
   Protected by TABLES_LOCK(). */
static hash_t *tracemalloc_traces = NULL;

#ifdef TRACE_ARENA
/* Set of arena pointers, see tracemalloc_alloc_arena().
   Protected by the GIL */
static hash_t *tracemalloc_arenas = NULL;
#endif

/* Forward declaration */
static int traceback_match_filters(traceback_t *traceback);

static void*
raw_malloc(size_t size)
{
    return allocators.raw.malloc(allocators.raw.ctx, size);
}

static void*
raw_realloc(void *ptr, size_t size)
{
    return allocators.raw.realloc(allocators.raw.ctx, ptr, size);
}

static void
raw_free(void *ptr)
{
    allocators.raw.free(allocators.raw.ctx, ptr);
}

#ifdef USE_MEMORY_POOL

#ifdef TRACE_ARENA
static void*
raw_alloc_arena(size_t size)
{
    return arena_allocator.alloc(arena_allocator.ctx, size);
}

static void
raw_free_arena(void *ptr, size_t size)
{
    arena_allocator.free(arena_allocator.ctx, ptr, size);
}
#else
static void*
raw_alloc_arena(size_t size)
{
    return malloc(size);
}

static void
raw_free_arena(void *ptr, size_t size)
{
    free(ptr);
}
#endif

#endif


static void
task_init(task_t *task)
{
    memset(task, 0,  sizeof(task_t));
    task->memory_threshold = -1;
    task->delay = -1;
}

static void
task_schedule(task_t *task)
{
    size_t traced;

    assert(!task->enabled);
    assert(task->ncall != 0);

    if (task->delay > 0) {
        task->timeout = time(NULL) + task->delay;
    }
    if (task->memory_threshold > 0) {
        TABLES_LOCK();
        traced = tracemalloc_traced_memory;
        TABLES_UNLOCK();

        if (traced >= task->memory_threshold)
            task->min_traced = traced - task->memory_threshold;
        else
            task->min_traced = 0;
        if (traced <= PY_SSIZE_T_MAX - task->memory_threshold)
            task->max_traced = traced + task->memory_threshold;
        else
            task->max_traced = PY_SSIZE_T_MAX;
    }
    task->failed = 0;
    task->enabled = 1;
}

static void
task_reschedule(task_t *task)
{
    if (task->failed)
        return;
    assert(task->enabled);
    task->enabled = 0;
    task_schedule(task);
}

static void
task_cancel(task_t *task)
{
    task->enabled = 0;
}

static PyObject*
task_call(task_t *task)
{
    return PyEval_CallObjectWithKeywords(task->func,
                                         task->func_args,
                                         task->func_kwargs);
}

static int
task_call_pending(void *user_data)
{
    task_t *task = user_data;
    PyObject *res;

    assert(task->ncall != 0);
    if (task->ncall > 0)
        task->ncall--;

    res = task_call(task);
    if (res == NULL) {
        /* on error, don't reschedule the task */
        task->failed = 1;
        PyErr_WriteUnraisable(NULL);
        return 0;
    }
    Py_DECREF(res);

    if (task->ncall != 0)
        task_schedule(task);

    return 0;
}

static void
task_call_later(task_t *task)
{
    int res;

    task->enabled = 0;
    res = Py_AddPendingCall(task_call_pending, task);
    if (res != 0) {
        /* failed to add the pending call: retry later */
        task->enabled = 1;
        return;
    }
}

static void
task_check(task_t *task)
{
    if (task->memory_threshold > 0
        && (tracemalloc_traced_memory <= task->min_traced
           || tracemalloc_traced_memory >= task->max_traced))
    {
        task_call_later(task);
        return;
    }

    if (task->delay > 0 && time(NULL) >= task->timeout)
    {
        task_call_later(task);
        return;
    }
}


static Py_uhash_t
key_hash_traceback(const void *key)
{
    const traceback_t *traceback = key;
    return traceback->hash;
}

static int
key_cmp_traceback(const traceback_t *traceback1, hash_entry_t *he)
{
    const traceback_t *traceback2 = he->key;
    const frame_t *frame1, *frame2;
    Py_hash_t hash1, hash2;
    int i;

    if (traceback1->nframe != traceback2->nframe)
        return 0;

    for (i=0; i < traceback1->nframe; i++) {
        frame1 = &traceback1->frames[i];
        frame2 = &traceback2->frames[i];

        if (frame1->lineno != frame2->lineno)
            return 0;

        if (frame1->filename != frame2->filename) {
            hash1 = filename_hash(frame1->filename);
            hash2 = filename_hash(frame2->filename);
            if (hash1 != hash2)
                return 0;

            if (STRING_COMPARE(frame1->filename, frame2->filename) != 0)
                return 0;
        }
    }
    return 1;
}

static void
tracemalloc_get_frame(PyFrameObject *pyframe, frame_t *frame)
{
    PyCodeObject *code;
    PyObject *filename;
    hash_entry_t *entry;

    frame->filename = NULL;
    frame->lineno = PyFrame_GetLineNumber(pyframe);

    code = pyframe->f_code;
    if (code == NULL) {
#ifdef TRACE_DEBUG
        tracemalloc_error(
            "failed to get the code object of "
            "the a frame (thread %li)",
            PyThread_get_thread_ident());
#endif
        return;
    }

    if (code->co_filename == NULL) {
#ifdef TRACE_DEBUG
        tracemalloc_error(
            "failed to get the filename of the code object "
            "(thread %li)",
            PyThread_get_thread_ident());
#endif
        return;
    }

    filename = code->co_filename;
    assert(filename != NULL);

    if (!STRING_CHECK(filename)) {
#ifdef TRACE_DEBUG
        tracemalloc_error("filename is not an unicode string");
#endif
        return;
    }

#ifdef PEP393
    if (!PyUnicode_IS_READY(filename)) {
        /* Don't make a Unicode string ready to avoid reentrant calls
           to tracemalloc_malloc() or tracemalloc_realloc() */
#ifdef TRACE_DEBUG
        tracemalloc_error("filename is not a ready unicode string");
#endif
        return;
    }
#endif

    /* intern the filename */
    entry = hash_get_entry(tracemalloc_filenames, filename);
    if (entry != NULL) {
        filename = (PyObject*)entry->key;
    }
    else {
        /* tracemalloc_filenames is responsible to keep a reference
           to the filename */
        Py_INCREF(filename);
        if (hash_put_data(tracemalloc_filenames, filename, NULL, 0) < 0) {
#ifdef TRACE_DEBUG
            tracemalloc_error("failed to intern the filename");
#endif
            return;
        }
    }

    /* the tracemalloc_filenames table keeps a reference to the filename */
    frame->filename = filename;
}

static Py_uhash_t
traceback_hash(traceback_t *traceback)
{
    /* code based on tuplehash() of Objects/tupleobject.c */
    Py_uhash_t x;  /* Unsigned for defined overflow behavior. */
    Py_hash_t y;
    int len = traceback->nframe;
    Py_uhash_t mult = _PyHASH_MULTIPLIER;
    frame_t *frame;

    x = 0x345678UL;
    frame = traceback->frames;
    while (--len >= 0) {
        y = filename_hash(frame->filename);
        y ^= frame->lineno;
        frame++;

        x = (x ^ y) * mult;
        /* the cast might truncate len; that doesn't change hash stability */
        mult += (Py_hash_t)(82520UL + len + len);
    }
    x += 97531UL;
    return x;
}

static void
traceback_get_frames(traceback_t *traceback)
{
    PyThreadState *tstate;
    PyFrameObject *pyframe;

    tstate = PyGILState_GetThisThreadState();
    if (tstate == NULL) {
#ifdef TRACE_DEBUG
        tracemalloc_error(
            "failed to get the current thread state (thread %li)",
            PyThread_get_thread_ident());
#endif
        return;
    }

    for (pyframe = tstate->frame; pyframe != NULL; pyframe = pyframe->f_back) {
        tracemalloc_get_frame(pyframe, &traceback->frames[traceback->nframe]);
        traceback->nframe++;
        if (traceback->nframe == tracemalloc_config.max_nframe)
            break;
    }
}

static traceback_t *
traceback_new(void)
{
    char stack_buffer[TRACEBACK_STACK_SIZE];
    traceback_t *traceback = (traceback_t *)stack_buffer;
    hash_entry_t *entry;

    assert(PyGILState_Check());
    traceback->nframe = 0;
    assert(tracemalloc_config.max_nframe > 0);
    traceback_get_frames(traceback);
    traceback->hash = traceback_hash(traceback);

    if (!traceback_match_filters(traceback))
        return NULL;

    /* intern the traceback */
    entry = hash_get_entry(tracemalloc_tracebacks, traceback);
    if (entry != NULL) {
        traceback = (traceback_t *)entry->key;
    }
    else {
        traceback_t *copy;
        size_t traceback_size;

        traceback_size = TRACEBACK_SIZE(traceback->nframe);

        copy = raw_malloc(traceback_size);
        if (copy == NULL) {
#ifdef TRACE_DEBUG
            tracemalloc_error("failed to intern the traceback: malloc failed");
#endif
            return NULL;
        }
        memcpy(copy, traceback, traceback_size);

        /* tracemalloc_tracebacks is responsible to keep a reference
           to the traceback */
        if (hash_put_data(tracemalloc_tracebacks, copy, NULL, 0) < 0) {
#ifdef TRACE_DEBUG
            tracemalloc_error("failed to intern the traceback: putdata failed");
#endif
            return NULL;
        }
        traceback = copy;
    }
    return traceback;
}

static void
tracemalloc_update_stats(trace_t *trace, int is_alloc)
{
    trace_stats_t local_trace_stat;
    trace_stats_t *trace_stats;
    hash_t *line_hash;
    void *line_key;
    hash_entry_t *line_entry;
    PyObject *filename;
    int lineno;
    traceback_t *traceback;

    traceback = trace->traceback;
    if (traceback->nframe >= 1) {
        filename = traceback->frames[0].filename;
        lineno = traceback->frames[0].lineno;
    }
    else {
        filename = NULL;
        lineno = -1;
    }

    if (!HASH_GET_DATA(tracemalloc_file_stats, filename, line_hash)) {
        if (!is_alloc) {
            /* clear_traces() was called or tracemalloc_update_stats() failed
               to store the allocation */
            return;
        }

        line_hash = hash_new(&tracemalloc_pools.traces,
                             sizeof(trace_stats_t),
                             key_hash_int, key_cmp_direct);
        if (line_hash == NULL) {
#ifdef TRACE_DEBUG
            tracemalloc_error("failed to allocate a hash table for lines "
                              "for a new filename");
#endif
            return;
        }
#ifdef PRINT_STATS
        line_hash->name = "line_stats";
#endif

        if (HASH_PUT_DATA(tracemalloc_file_stats, filename, line_hash) < 0) {
            hash_destroy(line_hash);
            return;
        }
    }

    line_key = INT_TO_POINTER(lineno);
    line_entry = hash_get_entry(line_hash, line_key);
    if (line_entry != NULL) {
        assert(line_hash->data_size == sizeof(trace_stats_t));
        trace_stats = (trace_stats_t *)ENTRY_DATA_PTR(line_entry);

        if (is_alloc) {
            assert(trace_stats->size < PY_SIZE_MAX - trace->size);
            trace_stats->size += trace->size;
            assert(trace_stats->count != PY_SIZE_MAX);
            trace_stats->count++;
        }
        else {
            assert(trace_stats->size >= trace->size);
            trace_stats->size -= trace->size;
            assert(trace_stats->count != 0);
            trace_stats->count--;
            assert(trace_stats->count != 0 || trace_stats->size == 0);

            if (trace_stats->count == 0) {
                hash_delete_data(line_hash, line_key);
                if (line_hash->entries == 0)
                    hash_delete_data(tracemalloc_file_stats, filename);
            }
        }
    }
    else {
        if (!is_alloc) {
            /* clear_traces() was called or tracemalloc_update_stats() failed
               to store the allocation */
            return;
        }

        local_trace_stat.size = trace->size;
        local_trace_stat.count = 1;
        HASH_PUT_DATA(line_hash, line_key, local_trace_stat);
    }
}

#ifdef TRACE_NORMALIZE_FILENAME
static Py_UCS4
tracemalloc_normalize_filename(Py_UCS4 ch)
{
#ifdef ALTSEP
    if (ch == ALTSEP)
        return SEP;
#endif
#ifdef TRACE_CASE_INSENSITIVE
    ch = CHAR_LOWER(ch);
#endif
    return ch;
}
#endif

typedef struct {
    PyObject *filename, *pattern;
#ifdef PEP393
    int file_kind, pat_kind;
    void *file_data, *pat_data;
#else
    char *file_data, *pat_data;
#endif
    Py_ssize_t file_len, pat_len;
} match_t;

static int
match_filename_joker(match_t *match, Py_ssize_t file_pos, Py_ssize_t pat_pos)
{
    Py_UCS4 ch1, ch2;

    while (file_pos < match->file_len && pat_pos < match->pat_len) {
        ch1 = STRING_READ(match->file_kind, match->file_data, file_pos);
        ch2 = STRING_READ(match->pat_kind, match->pat_data, pat_pos);
        if (ch2 == '*') {
            int found;

            do {
                pat_pos++;
                if (pat_pos >= match->pat_len) {
                    /* 'abc' always match '*' */
                    return 1;
                }
                ch2 = STRING_READ(match->pat_kind, match->pat_data, pat_pos);
            } while (ch2 == '*');

            do {
                found = match_filename_joker(match, file_pos, pat_pos);
                if (found)
                    break;
                file_pos++;
            } while (file_pos < match->file_len);

            return found;
        }

#ifdef TRACE_NORMALIZE_FILENAME
        ch1 = tracemalloc_normalize_filename(ch1);
#endif
        if (ch1 != ch2)
            return 0;

        file_pos++;
        pat_pos++;
    }

    if (pat_pos != match->pat_len) {
        if (pat_pos == (match->pat_len - 1)) {
            ch2 = STRING_READ(match->pat_kind, match->pat_data, pat_pos);
            if (ch2 == '*') {
                /* 'abc' matchs 'abc*' */
                return 1;
            }
        }
        return 0;
    }
    return 1;
}

static int
filename_endswith_pyc_pyo(PyObject *filename)
{
#ifdef PEP393
    void *data;
    int kind;
#elif defined(PYTHON3)
    Py_UNICODE *data;
#else
    char *data;
#endif
    Py_UCS4 ch;
    Py_ssize_t len;

    len = STRING_LENGTH(filename);
    if (len < 4)
        return 0;

#ifdef PEP393
    data = PyUnicode_DATA(filename);
    kind = PyUnicode_KIND(filename);
#elif defined(PYTHON3)
    data = PyUnicode_AS_UNICODE(filename);
#else
    data = PyString_AS_STRING(filename);
#endif

    if (STRING_READ(kind, data, len-4) != '.')
        return 0;
    ch = STRING_READ(kind, data, len-3);
#ifdef TRACE_CASE_INSENSITIVE
    ch = CHAR_LOWER(ch);
#endif
    if (ch != 'p')
        return 0;

    ch = STRING_READ(kind, data, len-2);
#ifdef TRACE_CASE_INSENSITIVE
    ch = CHAR_LOWER(ch);
#endif
    if (ch != 'y')
        return 0;

    ch = STRING_READ(kind, data, len-1);
#ifdef TRACE_CASE_INSENSITIVE
    ch = CHAR_LOWER(ch);
#endif
    if ((ch != 'c' && ch != 'o'))
        return 0;
    return 1;
}

static int
match_filename(filter_t *filter, PyObject *filename)
{
    Py_ssize_t len;
    match_t match;
    Py_hash_t hash;

#ifdef PEP393
    assert(PyUnicode_IS_READY(filename));
#endif

    if (filename == filter->pattern)
        return 1;

    hash = PyObject_Hash(filename);
    if (hash == filter->pattern_hash) {
        if (STRING_COMPARE(filename, filter->pattern) == 0)
            return 1;
    }

#ifndef TRACE_NORMALIZE_FILENAME
    if (!filter->use_joker) {
        if (!filename_endswith_pyc_pyo(filename)) {
            /* hash is different: strings are different */
            return 0;
        }
        else {
            len = STRING_LENGTH(filename);

            /* don't compare last character */
            return PyUnicode_Tailmatch(filename, filter->pattern,
                                       0, len - 1, 1);
        }
    }
#endif

    len = STRING_LENGTH(filename);

    /* replace "a.pyc" and "a.pyo" with "a.py" */
    if (filename_endswith_pyc_pyo(filename))
        len--;

    match.filename = filename;
#ifdef PEP393
    match.file_kind = PyUnicode_KIND(match.filename);
#endif
    match.file_data = STRING_DATA(match.filename);
    match.file_len = len;

    match.pattern = filter->pattern;
#ifdef PEP393
    match.pat_kind = PyUnicode_KIND(match.pattern);
#endif
    match.pat_data = STRING_DATA(match.pattern);
    match.pat_len = STRING_LENGTH(match.pattern);
    return match_filename_joker(&match, 0, 0);
}

static int
filter_match_filename(filter_t *filter, PyObject *filename)
{
    int match;

    if (filename == NULL)
        return !filter->include;

    match = match_filename(filter, filename);
    return match ^ !filter->include;
}

static int
filter_match_lineno(filter_t *filter, int lineno)
{
    int match;

    if (filter->lineno < 1)
        return 1;

    if (lineno < 1)
        return !filter->include;

    match = (lineno == filter->lineno);
    return match ^ !filter->include;
}

static int
filter_match(filter_t *filter, PyObject *filename, int lineno)
{
    int match;

    if (filename == NULL)
        return !filter->include;

    match = match_filename(filter, filename);
    if (filter->include) {
        if (!match)
            return 0;
    }
    else {
        /* exclude */
        if (!match)
            return 1;
        else if (filter->lineno < 1)
            return 0;
    }

    return filter_match_lineno(filter, lineno);
}

static int
filter_match_traceback(filter_t *filter, traceback_t *traceback)
{
    int i;
    PyObject *filename;
    int lineno;
    int nframe;
    int match;

    nframe = traceback->nframe;
    if (nframe == 0) {
        return filter_match(filter, NULL, -1);
    }
    else if (!filter->traceback) {
        filename = traceback->frames[0].filename;
        lineno = traceback->frames[0].lineno;
        return filter_match(filter, filename, lineno);
    }
    else {
        for (i = 0; i < nframe; i++) {
            filename = traceback->frames[i].filename;
            lineno = traceback->frames[i].lineno;

            match = filter_match(filter, filename, lineno);
            if (match) {
                if (filter->include)
                    return 1;
            }
            else {
                if (!filter->include)
                    return 0;
            }
        }
        return !filter->include;
    }
}

static int
filter_list_match(filter_list_t *filters, int include, traceback_t *traceback)
{
    size_t i;
    filter_t *filter;
    int match;

    if (filters->nfilter == 0)
        return 1;

    for (i = 0; i < filters->nfilter; i++) {
        filter = &filters->filters[i];

        match = filter_match_traceback(filter, traceback);
        if (include) {
            if (match)
                return 1;
        }
        else {
            if (!match)
                return 0;
        }
    }
    return !include;
}

static int
traceback_match_filters(traceback_t *traceback)
{
    if (!filter_list_match(&tracemalloc_include_filters, 1, traceback))
        return 0;
    if (!filter_list_match(&tracemalloc_exclude_filters, 0, traceback))
        return 0;
    return 1;
}

static void
tracemalloc_log_alloc(void *ptr, size_t size)
{
    traceback_t *traceback;
    trace_t trace;

    assert(PyGILState_Check());

    if (tracemalloc_config.max_nframe > 0) {
        traceback = traceback_new();
        if (traceback == NULL)
            return;
    }
    else {
        traceback = &tracemalloc_empty_traceback;
        if (!traceback_match_filters(traceback))
            return;
    }

    trace.size = size;
    trace.traceback = traceback;

    TABLES_LOCK();
    assert(tracemalloc_traced_memory <= PY_SIZE_MAX - size);
    tracemalloc_traced_memory += size;
    if (tracemalloc_traced_memory > tracemalloc_max_traced_memory)
        tracemalloc_max_traced_memory = tracemalloc_traced_memory;

    tracemalloc_update_stats(&trace, 1);
    HASH_PUT_DATA(tracemalloc_traces, ptr, trace);
    TABLES_UNLOCK();
}

static void
tracemalloc_log_free(void *ptr)
{
    trace_t trace;

    TABLES_LOCK();
    if (hash_pop_data(tracemalloc_traces, ptr, &trace, sizeof(trace))) {
        assert(tracemalloc_traced_memory >= trace.size);
        tracemalloc_traced_memory -= trace.size;
        tracemalloc_update_stats(&trace, 0);
    }
    TABLES_UNLOCK();
}

static void*
tracemalloc_malloc(void *ctx, size_t size, int gil_held)
{
    PyMemAllocator *alloc = (PyMemAllocator *)ctx;
#ifdef TRACE_RAW_MALLOC
    PyGILState_STATE gil_state;
#endif
    void *ptr;

    if (get_reentrant())
        return alloc->malloc(alloc->ctx, size);

#ifdef TRACE_RAW_MALLOC
    if (!gil_held) {
        /* PyGILState_Ensure() may call PyMem_RawMalloc() indirectly which
           would call PyGILState_Ensure() if reentrant are not disabled. */
        set_reentrant(1);
        gil_state = PyGILState_Ensure();

        ptr = alloc->malloc(alloc->ctx, size);
        set_reentrant(0);
    }
    else
#endif
    {
        assert(gil_held);

        /* Ignore reentrant call: PyObjet_Malloc() calls PyMem_Malloc() for
           allocations larger than 512 bytes */
        set_reentrant(1);
        ptr = alloc->malloc(alloc->ctx, size);
        set_reentrant(0);
    }

    if (ptr != NULL)
        tracemalloc_log_alloc(ptr, size);

    task_list_check();

#ifdef TRACE_RAW_MALLOC
    if (!gil_held)
        PyGILState_Release(gil_state);
#endif

    return ptr;
}

static void*
tracemalloc_realloc(void *ctx, void *ptr, size_t new_size, int gil_held)
{
    PyMemAllocator *alloc = (PyMemAllocator *)ctx;
#ifdef TRACE_RAW_MALLOC
    PyGILState_STATE gil_state;
#endif
    void *ptr2;

    if (get_reentrant()) {
        /* Reentrant call to PyMem_Realloc() and PyMem_RawRealloc().
           Example: PyMem_RawRealloc() is called internally by pymalloc
           (_PyObject_Malloc() and  _PyObject_Realloc()) to allocate a new
           arena (new_arena()). */
        ptr2 = alloc->realloc(alloc->ctx, ptr, new_size);


        if (ptr2 != NULL && ptr != NULL) {
#ifdef TRACE_RAW_MALLOC
            if (!gil_held) {
                gil_state = PyGILState_Ensure();
            tracemalloc_log_free(ptr);
                PyGILState_Release(gil_state);
            }
            else
#endif
            {
                assert(gil_held);
                tracemalloc_log_free(ptr);
            }
        }

        return ptr2;
    }

#ifdef TRACE_RAW_MALLOC
    if (!gil_held) {
        /* PyGILState_Ensure() may call PyMem_RawMalloc() indirectly which
           would call PyGILState_Ensure() if reentrant are not disabled. */
        set_reentrant(1);
        gil_state = PyGILState_Ensure();

        ptr2 = alloc->realloc(alloc->ctx, ptr, new_size);
        set_reentrant(0);
    }
    else
#endif
    {
        assert(gil_held);

        /* PyObjet_Realloc() calls PyMem_Realloc() for allocations
           larger than 512 bytes */
        set_reentrant(1);
        ptr2 = alloc->realloc(alloc->ctx, ptr, new_size);
        set_reentrant(0);
    }

    if (ptr2 != NULL) {
        if (ptr != NULL)
            tracemalloc_log_free(ptr);

        tracemalloc_log_alloc(ptr2, new_size);
    }

    task_list_check();

#ifdef TRACE_RAW_MALLOC
    if (!gil_held)
        PyGILState_Release(gil_state);
#endif

    return ptr2;
}

static void
tracemalloc_free(void *ctx, void *ptr, int gil_held)
{
    PyMemAllocator *alloc = (PyMemAllocator *)ctx;

     /* Cannot lock the GIL in PyMem_RawFree() because it would introduce
        a deadlock in PyThreadState_DeleteCurrent(). */

    if (ptr != NULL) {
        alloc->free(alloc->ctx, ptr);
        tracemalloc_log_free(ptr);
    }

    if (gil_held) {
        /* tracemalloc_task is protected by the GIL */
        task_list_check();
    }
}

static void
tracemalloc_free_gil(void *ctx, void *ptr)
{
    tracemalloc_free(ctx, ptr, 1);
}

static void
tracemalloc_raw_free(void *ctx, void *ptr)
{
    tracemalloc_free(ctx, ptr, 0);
}

static void*
tracemalloc_malloc_gil(void *ctx, size_t size)
{
    return tracemalloc_malloc(ctx, size, 1);
}

static void*
tracemalloc_realloc_gil(void *ctx, void *ptr, size_t new_size)
{
    return tracemalloc_realloc(ctx, ptr, new_size, 1);
}

#ifdef TRACE_RAW_MALLOC
static void*
tracemalloc_raw_malloc(void *ctx, size_t size)
{
    return tracemalloc_malloc(ctx, size, 0);
}

static void*
tracemalloc_raw_realloc(void *ctx, void *ptr, size_t new_size)
{
    return tracemalloc_realloc(ctx, ptr, new_size, 0);
}
#endif

#ifdef TRACE_ARENA
static void*
tracemalloc_alloc_arena(void *ctx, size_t size)
{
    void *ptr;

    ptr = arena_allocator.alloc(ctx, size);
    if (ptr == NULL)
        return NULL;

    if (hash_put_data(tracemalloc_arenas, ptr, NULL, 0) == 0) {
        assert(tracemalloc_arena_size <= PY_SIZE_MAX - size);
        tracemalloc_arena_size += size;
    }
    else {
#ifdef TRACE_DEBUG
        tracemalloc_error("alloc_arena: put_data() failed");
#endif
    }
    return ptr;
}

static void
tracemalloc_free_arena(void *ctx, void *ptr, size_t size)
{
    arena_allocator.free(ctx, ptr, size);
    if (hash_may_delete_data(tracemalloc_arenas, ptr)) {
        assert(tracemalloc_arena_size >= size);
        tracemalloc_arena_size -= size;
    }
}
#endif

static int
filter_init(filter_t *filter,
            int include, PyObject *pattern, int lineno,
            int traceback)
{
    Py_ssize_t len, len2;
    Py_ssize_t i, j;
    PyObject *new_pattern;
    Py_UCS4 ch;
#ifdef PEP393
    Py_UCS4 maxchar;
    int kind, kind2;
    void *data, *data2;
#elif defined(PYTHON3)
    Py_UNICODE *data, *data2;
#else
    char *data, *data2;
#endif
    int previous_joker;
    size_t njoker;
    Py_hash_t pattern_hash;

#ifdef PYTHON3
    if (!STRING_CHECK(pattern)) {
        PyErr_Format(PyExc_TypeError,
                     "filename pattern must be a str, not %s",
                     Py_TYPE(pattern)->tp_name);
        return -1;
    }
#else
    if (!STRING_CHECK(pattern)) {
        PyErr_Format(PyExc_TypeError,
                     "filename pattern must be a str, not %s",
                     Py_TYPE(pattern)->tp_name);
        return -1;
    }
#endif

#ifdef PEP393
    if (PyUnicode_READY(pattern) < 0)
        return -1;
#endif

    len = STRING_LENGTH(pattern);
    data = STRING_DATA(pattern);
#ifdef PEP393
    kind = PyUnicode_KIND(pattern);
#endif

    if (filename_endswith_pyc_pyo(pattern))
        len--;

#ifdef PEP393
    maxchar = 0;
#endif
    len2 = 0;
    njoker = 0;
    previous_joker = 0;
    for (i=0; i < len; i++) {
        ch = STRING_READ(kind, data, i);
#ifdef TRACE_NORMALIZE_FILENAME
        ch = tracemalloc_normalize_filename(ch);
#endif
        if (!previous_joker || ch != '*') {
            previous_joker = (ch == '*');
            if (previous_joker)
                njoker++;
#ifdef PEP393
            maxchar = Py_MAX(maxchar, ch);
#endif
            len2++;
        }
        else {
            /* skip consecutive joker character */
        }
    }

    if (njoker > MAX_NJOKER) {
        PyErr_SetString(PyExc_ValueError,
                        "too many joker characters in the filename pattern");
        return -1;
    }

#ifdef PEP393
    new_pattern = PyUnicode_New(len2, maxchar);
#elif defined(PYTHON3)
    new_pattern = PyUnicode_New(len2);
#else
    new_pattern = PyString_FromStringAndSize(NULL, len2);
#endif
    if (new_pattern == NULL)
        return -1;
#ifdef PEP393
    kind2 = PyUnicode_KIND(new_pattern);
#endif
    data2 = STRING_DATA(new_pattern);

    j = 0;
    previous_joker = 0;
    for (i=0; i < len; i++) {
        ch = STRING_READ(kind, data, i);
#ifdef TRACE_NORMALIZE_FILENAME
        ch = tracemalloc_normalize_filename(ch);
#endif
        if (!previous_joker || ch != '*') {
            previous_joker = (ch == '*');
            STRING_WRITE(kind2, data2, j, ch);
            j++;
        }
        else {
            /* skip consecutive joker character */
        }
    }
    assert(j == len2);

    assert(_PyUnicode_CheckConsistency(new_pattern, 1));

    pattern = new_pattern;

    pattern_hash = PyObject_Hash(pattern);

    filter->include = include;
    filter->pattern_hash = pattern_hash;
    filter->pattern = pattern;
#ifndef TRACE_NORMALIZE_FILENAME
    filter->use_joker = (njoker != 0);
#endif
    if (lineno >= 1)
        filter->lineno = lineno;
    else
        filter->lineno = -1;
    filter->traceback = traceback;
    return 0;
}

static void
filter_deinit(filter_t *filter)
{
    Py_CLEAR(filter->pattern);
}

static void
filter_list_init(filter_list_t *filters)
{
    filters->nfilter = 0;
    filters->filters = NULL;
}

static void
filter_list_clear(filter_list_t *filters)
{
    size_t i;

    if (filters->nfilter == 0) {
        assert(filters->filters == NULL);
        return;
    }

    for (i=0; i<filters->nfilter; i++)
        filter_deinit(&filters->filters[i]);

    filters->nfilter = 0;
    raw_free(filters->filters);
    filters->filters = NULL;
}

static void
tracemalloc_clear_filters(void)
{
    filter_list_clear(&tracemalloc_include_filters);
    filter_list_clear(&tracemalloc_exclude_filters);
}

static int
tracemalloc_clear_filename(hash_entry_t *entry, void *user_data)
{
    PyObject *filename = (PyObject *)entry->key;
    Py_DECREF(filename);
    return 0;
}

static int
traceback_free_cb(hash_entry_t *entry, void *user_data)
{
    traceback_t *traceback = (traceback_t *)entry->key;
    raw_free(traceback);
    return 0;
}

/* reentrant flag must be set to call this function and GIL must be held */
static void
tracemalloc_clear_traces(void)
{
    /* The GIL protects variables againt concurrent access */
    assert(PyGILState_Check());

    /* Disable also reentrant calls to tracemalloc_malloc() to not add a new
       trace while we are clearing traces */
    assert(get_reentrant());

    TABLES_LOCK();
    hash_clear(tracemalloc_file_stats);
    hash_clear(tracemalloc_traces);
    tracemalloc_traced_memory = 0;
    tracemalloc_max_traced_memory = 0;
    TABLES_UNLOCK();

    hash_foreach(tracemalloc_tracebacks, traceback_free_cb, NULL);
    hash_clear(tracemalloc_tracebacks);

    hash_foreach(tracemalloc_filenames, tracemalloc_clear_filename, NULL);
    hash_clear(tracemalloc_filenames);

#ifdef TRACE_ARENA
    hash_clear(tracemalloc_arenas);
    tracemalloc_arena_size = 0;
#endif
}

static int
tracemalloc_init(void)
{
    size_t entry_size;
#ifdef TRACE_ATFORK
    int res;
#endif

    /* ensure that the frame_t structure is packed */
    assert(sizeof(frame_t) == (sizeof(PyObject*) + sizeof(int)));

    if (tracemalloc_config.init)
        return 0;

    tracemalloc_tasks = PyList_New(0);
    if (tracemalloc_tasks == NULL)
        return -1;

    PyMem_GetAllocator(PYMEM_DOMAIN_RAW, &allocators.raw);
#ifdef TRACE_ARENA
    PyObject_GetArenaAllocator(&arena_allocator);
#endif

#ifdef REENTRANT_THREADLOCAL
#ifdef NT_THREADS
    tracemalloc_reentrant_key = TlsAlloc();
    if (tracemalloc_reentrant_key == TLS_OUT_OF_INDEXES) {
        PyErr_SetFromWindowsErr(0);
        return -1;
    }
#else
    if (pthread_key_create(&tracemalloc_reentrant_key, NULL)) {
        PyErr_SetFromErrno(PyExc_OSError);
        return -1;
    }
#endif
#endif

    filter_list_init(&tracemalloc_include_filters);
    filter_list_init(&tracemalloc_exclude_filters);

#if defined(TRACE_RAW_MALLOC) && defined(WITH_THREAD)
    if (tables_lock == NULL) {
        tables_lock = PyThread_allocate_lock();
        if (tables_lock == NULL) {
            PyErr_SetString(PyExc_RuntimeError, "cannot allocate lock");
            return -1;
        }
    }
#endif

    entry_size = sizeof(hash_entry_t);
    pool_init(&tracemalloc_pools.no_data, entry_size + 0);
    assert(sizeof(trace_t) == sizeof(trace_stats_t));
    pool_init(&tracemalloc_pools.traces, entry_size + sizeof(trace_t));
    pool_init(&tracemalloc_pools.hash_tables, entry_size + sizeof(hash_t*));

    tracemalloc_filenames = hash_new(&tracemalloc_pools.no_data,
                                     0,
                                     (key_hash_func)filename_hash,
                                     key_cmp_unicode);

    tracemalloc_tracebacks = hash_new(&tracemalloc_pools.no_data,
                                      0,
                                      (key_hash_func)key_hash_traceback,
                                      (key_compare_func)key_cmp_traceback);

    tracemalloc_file_stats = hash_new_full(&tracemalloc_pools.hash_tables,
                                           sizeof(hash_t *),
                                           0,
                                           key_hash_ptr,
                                           key_cmp_direct,
                                           (hash_copy_data_func)hash_copy,
                                           (hash_free_data_func)hash_destroy,
                                           (hash_get_data_size_func)hash_mem_stats);

    tracemalloc_traces = hash_new(&tracemalloc_pools.traces,
                                  sizeof(trace_t),
                                  key_hash_ptr, key_cmp_direct);

#ifdef TRACE_ARENA
    tracemalloc_arenas = hash_new(&tracemalloc_pools.no_data,
                                  0,
                                  key_hash_arena_ptr, key_cmp_direct);
#endif

    if (tracemalloc_filenames == NULL || tracemalloc_tracebacks == NULL
        || tracemalloc_file_stats == NULL || tracemalloc_traces == NULL
#ifdef TRACE_ARENA
        || tracemalloc_arenas == NULL
#endif
        )
    {
        PyErr_NoMemory();
        return -1;
    }
#ifdef PRINT_STATS
    tracemalloc_pools.no_data.name = "no data";
    tracemalloc_pools.traces.name = "traces";
    tracemalloc_pools.hash_tables.name = "hash_tables";

    tracemalloc_filenames->name = "filenames";
    tracemalloc_tracebacks->name = "tracebacks";
    tracemalloc_file_stats->name = "file_stats";
    tracemalloc_traces->name = "traces";
#ifdef TRACE_ARENA
    tracemalloc_arenas->name = "arenas";
#endif
#endif

#ifdef TRACE_ATFORK
    res = pthread_atfork(NULL, NULL, tracemalloc_atfork);
    if (res != 0) {
        PyErr_SetFromErrno(PyExc_OSError);
        return -1;
    }
#endif

    tracemalloc_empty_traceback.nframe = 0;
    tracemalloc_empty_traceback.hash = traceback_hash(&tracemalloc_empty_traceback);

    /* disable tracing allocations until tracemalloc is fully initialized */
    set_reentrant(1);

    tracemalloc_config.init = 1;
    return 0;
}

static void
tracemalloc_deinit(void)
{
    int err;
#if defined(REENTRANT_THREADLOCAL) && !defined(NDEBUG)
#ifdef NT_THREADS
    BOOL res;
#else
    int res;
#endif
#endif

    if (!tracemalloc_config.init)
        return;
    tracemalloc_config.init = 0;

    err = tracemalloc_disable();
    assert(err == 0);

    tracemalloc_clear_filters();

    Py_CLEAR(tracemalloc_tasks);

    /* destroy hash tables */
    hash_destroy(tracemalloc_file_stats);
    hash_destroy(tracemalloc_traces);
    hash_destroy(tracemalloc_tracebacks);
    hash_destroy(tracemalloc_filenames);
#ifdef TRACE_ARENA
    hash_destroy(tracemalloc_arenas);
#endif

#ifdef USE_MEMORY_POOL
    pool_clear(&tracemalloc_pools.no_data);
    pool_clear(&tracemalloc_pools.traces);
    pool_clear(&tracemalloc_pools.hash_tables);
#endif

#if defined(WITH_THREAD) && defined(TRACE_RAW_MALLOC)
    if (tables_lock == NULL) {
        PyThread_free_lock(tables_lock);
        tables_lock = NULL;
    }
#endif

#ifdef REENTRANT_THREADLOCAL
#ifdef NT_THREADS
    /* Windows */
    if (tracemalloc_reentrant_key != TLS_OUT_OF_INDEXES) {
#ifndef NDEBUG
        res = TlsFree(tracemalloc_reentrant_key);
        assert(res);
#else
        (void)TlsFree(tracemalloc_reentrant_key);
#endif
    }
#else
    /* pthread */
#ifndef NDEBUG
    res = pthread_key_delete(tracemalloc_reentrant_key);
    assert(res == 0);
#else
    (void)pthread_key_delete(tracemalloc_reentrant_key);
#endif
#endif
#endif
}

static int
tracemalloc_enable(void)
{
    PyMemAllocator alloc;
#ifdef TRACE_ARENA
    PyObjectArenaAllocator arena_hook;
#endif

    if (tracemalloc_init() < 0)
        return -1;

    if (tracemalloc_config.enabled) {
        /* hook already installed: do nothing */
        return 0;
    }

    tracemalloc_traced_memory = 0;
    tracemalloc_max_traced_memory = 0;
#ifdef TRACE_ARENA
    tracemalloc_arena_size = 0;
#endif

#ifdef TRACE_RAW_MALLOC
    alloc.malloc = tracemalloc_raw_malloc;
    alloc.realloc = tracemalloc_raw_realloc;
    alloc.free = tracemalloc_raw_free;

    alloc.ctx = &allocators.raw;
    PyMem_GetAllocator(PYMEM_DOMAIN_RAW, &allocators.raw);
    PyMem_SetAllocator(PYMEM_DOMAIN_RAW, &alloc);
#endif

    alloc.malloc = tracemalloc_malloc_gil;
    alloc.realloc = tracemalloc_realloc_gil;
    alloc.free = tracemalloc_free_gil;

    alloc.ctx = &allocators.mem;
    PyMem_GetAllocator(PYMEM_DOMAIN_MEM, &allocators.mem);
    PyMem_SetAllocator(PYMEM_DOMAIN_MEM, &alloc);

    alloc.ctx = &allocators.obj;
    PyMem_GetAllocator(PYMEM_DOMAIN_OBJ, &allocators.obj);
    PyMem_SetAllocator(PYMEM_DOMAIN_OBJ, &alloc);

#ifdef TRACE_ARENA
    arena_hook.ctx = &arena_allocator;
    arena_hook.alloc = tracemalloc_alloc_arena;
    arena_hook.free = tracemalloc_free_arena;
    PyObject_SetArenaAllocator(&arena_hook);
#endif

    /* every is ready: start tracing Python memory allocations */
    set_reentrant(0);

    tracemalloc_config.enabled = 1;
    return 0;
}

static int
tracemalloc_disable(void)
{
    int err;

    if (!tracemalloc_config.enabled)
        return 0;

    /* cancel all scheduled tasks */
    err = task_list_clear();

    /* stop tracing Python memory allocations */
    set_reentrant(1);

    tracemalloc_config.enabled = 0;

    /* unregister the hook on memory allocators */
#ifdef TRACE_RAW_MALLOC
    PyMem_SetAllocator(PYMEM_DOMAIN_RAW, &allocators.raw);
#endif
    PyMem_SetAllocator(PYMEM_DOMAIN_MEM, &allocators.mem);
    PyMem_SetAllocator(PYMEM_DOMAIN_OBJ, &allocators.obj);

#ifdef TRACE_ARENA
    PyObject_SetArenaAllocator(&arena_allocator);
#endif

    /* release memory */
    tracemalloc_clear_traces();
    return err;
}

#ifdef TRACE_ATFORK
static void
tracemalloc_atfork(void)
{
    int err;

    PyGILState_STATE gil_state;

    if (!tracemalloc_config.enabled)
        return;

    /* fork() can be called with the GIL released */
    set_reentrant(1);
    gil_state = PyGILState_Ensure();
    set_reentrant(0);

    err = tracemalloc_disable();
    assert(err == 0);
    PyGILState_Release(gil_state);
}
#endif

typedef struct {
    pool_t pool;
    hash_t *file_stats;
    hash_t *line_hash;
    PyObject *file_dict;
    PyObject *line_dict;
} get_stats_t;

static PyObject*
tracemalloc_lineno_as_obj(int lineno)
{
    if (lineno > 0)
        return PyLong_FromLong(lineno);
    else
        Py_RETURN_NONE;
}

static PyObject*
trace_stats_to_pyobject(trace_stats_t *trace_stats)
{
    PyObject *size, *count, *line_obj;

    line_obj = PyTuple_New(2);
    if (line_obj == NULL)
        return NULL;

    size = PyLong_FromSize_t(trace_stats->size);
    if (size == NULL) {
        Py_DECREF(line_obj);
        return NULL;
    }
    PyTuple_SET_ITEM(line_obj, 0, size);

    count = PyLong_FromSize_t(trace_stats->count);
    if (count == NULL) {
        Py_DECREF(line_obj);
        return NULL;
    }
    PyTuple_SET_ITEM(line_obj, 1, count);

    return line_obj;
}

static int
tracemalloc_get_stats_fill_line(hash_entry_t *entry, void *user_data)
{
    int lineno;
    trace_stats_t trace_stats;
    get_stats_t *get_stats = user_data;
    PyObject *key, *line_obj;
    int err;

    lineno = POINTER_TO_INT(entry->key);

    HASH_ENTRY_READ_DATA(get_stats->line_hash,
                         &trace_stats, sizeof(trace_stats), entry);

    key = tracemalloc_lineno_as_obj(lineno);
    if (key == NULL)
        return 1;

    line_obj = trace_stats_to_pyobject(&trace_stats);
    if (line_obj == NULL) {
        Py_DECREF(key);
        return 1;
    }

    err = PyDict_SetItem(get_stats->line_dict, key, line_obj);
    Py_DECREF(key);
    Py_DECREF(line_obj);
    return err;
}

static int
tracemalloc_get_stats_fill_file(hash_entry_t *entry, void *user_data)
{
    PyObject *filename;
    get_stats_t *get_stats = user_data;
    int res;

    filename = (PyObject *)entry->key;
    if (filename == NULL)
        filename = Py_None;

    get_stats->line_hash = HASH_ENTRY_DATA_AS_VOID_P(entry);

    get_stats->line_dict = PyDict_New();
    if (get_stats->line_dict == NULL)
        return 1;

    res = hash_foreach(get_stats->line_hash,
                       tracemalloc_get_stats_fill_line, user_data);
    if (res) {
        Py_DECREF(get_stats->line_dict);
        return 1;
    }

    res = PyDict_SetItem(get_stats->file_dict, filename, get_stats->line_dict);
    Py_CLEAR(get_stats->line_dict);
    if (res < 0)
        return 1;

    return 0;
}

typedef struct {
    PyObject_HEAD
    filter_t filter;
} FilterObject;

/* Converter for PyArg_ParseTuple() to parse a filename, accepting None */
static int
tracemalloc_parse_filename(PyObject* arg, void* addr)
{
    PyObject *filename;

    if (arg == Py_None) {
        filename = NULL;
    }
    else if (STRING_CHECK(arg)) {
        filename = arg;
    }
    else {
        PyErr_SetString(PyExc_TypeError, "filename must be a str or None");
        return 0;
    }
    *(PyObject **)addr = filename;
    return 1;
}

/* Converter for PyArg_ParseTuple() to parse a line number, accepting None */
static int
tracemalloc_parse_lineno(PyObject* arg, void* addr)
{
    int lineno;

    if (arg == Py_None) {
        lineno = -1;
    }
    else {
        lineno = _PyLong_AsInt(arg);
        if (lineno == -1 && PyErr_Occurred())
            return 0;
    }
    *(int *)addr = lineno;
    return 1;
}

static int
pyfilter_init(FilterObject *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"include", "filename", "lineno", "traceback", 0};
    int include;
    PyObject *filename;
    int lineno = -1;
    int traceback = 0;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "iO|O&i:str", kwlist,
                                     &include, &filename,
                                     tracemalloc_parse_lineno, &lineno,
                                     &traceback))
        return -1;

    filter_deinit(&self->filter);

    if (filter_init(&self->filter, include, filename, lineno, traceback) < 0)
        return -1;

    return 0;
}

static void
pyfilter_dealloc(FilterObject *self)
{
    filter_deinit(&self->filter);
    PyObject_FREE(self);
}

static PyObject*
pyfilter_match(PyObject *self, PyObject *args)
{
    FilterObject *pyfilter = (FilterObject *)self;
    PyObject *filename;
    int lineno;
    int match;

    if (!PyArg_ParseTuple(args, "O&O&:match",
                          tracemalloc_parse_filename, &filename,
                          tracemalloc_parse_lineno, &lineno))
        return NULL;

    match = filter_match(&pyfilter->filter, filename, lineno);
    return PyBool_FromLong(match);
}

static int
parse_traceback(PyObject *pytraceback,
                traceback_t *traceback, size_t buffer_size)
{
    PyObject *pyframe, *filename, *pylineno;
    Py_ssize_t nframe, i;
    int lineno;

    if (!PyTuple_Check(pytraceback)) {
        PyErr_SetString(PyExc_TypeError, "traceback must be a tuple");
        return -1;
    }

    nframe = PyTuple_GET_SIZE(pytraceback);
    if (nframe > MAX_NFRAME) {
        PyErr_SetString(PyExc_TypeError, "too many frames");
        return -1;
    }
    assert(TRACEBACK_SIZE(nframe) <= buffer_size);

    traceback->nframe = nframe;
    for (i=0; i < nframe; i++) {
        pyframe = PyTuple_GET_ITEM(pytraceback, i);
        assert(pyframe != NULL);
        if (!PyTuple_Check(pyframe) || Py_SIZE(pyframe) != 2) {
            PyErr_SetString(PyExc_TypeError, "frames must be 2-tuples");
            return -1;
        }

        filename = PyTuple_GET_ITEM(pyframe, 0);
        assert(filename != NULL);
        pylineno = PyTuple_GET_ITEM(pyframe, 1);
        assert(pylineno != NULL);
        if (tracemalloc_parse_lineno(pylineno, &lineno) == 0)
            return -1;

        /* borrowed reference to filename */
        traceback->frames[i].filename = filename;
        traceback->frames[i].lineno = lineno;
    }

    return 0;
}

static PyObject*
pyfilter_match_traceback(PyObject *self, PyObject *args)
{
    FilterObject *pyfilter = (FilterObject *)self;
    PyObject *pytraceback;
    int match;
    char stack_buffer[TRACEBACK_STACK_SIZE];
    traceback_t *traceback = (traceback_t *)stack_buffer;

    if (!PyArg_ParseTuple(args, "O:match_traceback", &pytraceback))
        return NULL;

    if (parse_traceback(pytraceback, traceback, sizeof(stack_buffer)) < 0)
        return NULL;

    match = filter_match_traceback(&pyfilter->filter, traceback);
    return PyBool_FromLong(match);
}

static PyObject*
pyfilter_match_filename(PyObject *self, PyObject *args)
{
    FilterObject *pyfilter = (FilterObject *)self;
    PyObject *filename;
    int match;

    if (!PyArg_ParseTuple(args, "O&:match_filename",
                          tracemalloc_parse_filename, &filename))
        return NULL;

    match = filter_match_filename(&pyfilter->filter, filename);
    return PyBool_FromLong(match);
}

static PyObject*
pyfilter_match_lineno(PyObject *self, PyObject *args)
{
    FilterObject *pyfilter = (FilterObject *)self;
    int lineno;
    int match;

    if (!PyArg_ParseTuple(args, "O&:match_lineno",
                          tracemalloc_parse_lineno, &lineno))
        return NULL;

    match = filter_match_lineno(&pyfilter->filter, lineno);
    return PyBool_FromLong(match);
}

static PyObject *
pyfilter_get_include(FilterObject *self, void *closure)
{
    return PyBool_FromLong(self->filter.include);
}

static PyObject *
pyfilter_get_pattern(FilterObject *self, void *closure)
{
    Py_INCREF(self->filter.pattern);
    return self->filter.pattern;
}

static PyObject *
pyfilter_get_lineno(FilterObject *self, void *closure)
{
    return tracemalloc_lineno_as_obj(self->filter.lineno);
}

static PyObject *
pyfilter_get_traceback(FilterObject *self, void *closure)
{
    return PyBool_FromLong(self->filter.traceback);
}

static PyObject*
pyfilter_repr(FilterObject *self)
{
    char lineno[30];
    if (self->filter.lineno > 1)
        PyOS_snprintf(lineno, sizeof(lineno), "%i", self->filter.lineno);
    else
        strcpy(lineno, "None");
    return PyUnicode_FromFormat("<tracemalloc.Filter include=%s pattern=%R lineno=%s traceback=%s>",
                                self->filter.include ? "True" : "False",
                                self->filter.pattern,
                                lineno,
                                self->filter.traceback ? "True" : "False");
}

static int
filter_compare(filter_t *f1, filter_t *f2)
{
    if (f1->include != f2->include)
        return 0;
    if (f1->lineno != f2->lineno)
        return 0;
    if (f1->traceback != f2->traceback)
        return 0;
    if (STRING_COMPARE(f1->pattern, f2->pattern) != 0)
        return 0;
#ifndef TRACE_NORMALIZE_FILENAME
    assert(f1->use_joker == f2->use_joker);
#endif
    return 1;
}

static Py_hash_t
pyfilter_hash(FilterObject *self)
{
    Py_hash_t hash;

    hash = PyObject_Hash(self->filter.pattern);
    hash ^= self->filter.lineno;
    hash ^= ((Py_hash_t)self->filter.include << 20);
    hash ^= ((Py_hash_t)self->filter.traceback << 21);
    return hash;
}

static PyObject *
pyfilter_richcompare(FilterObject *self, FilterObject *other, int op)
{
    if (op == Py_EQ || op == Py_NE) {
        int eq;
        PyObject *res;

        eq = filter_compare(&self->filter, &other->filter);
        if (op == Py_NE)
            eq = !eq;

        if (eq)
            res = Py_True;
        else
            res = Py_False;
        Py_INCREF(res);
        return res;
    }
    else {
        Py_INCREF(Py_NotImplemented);
        return Py_NotImplemented;
    }
}

static PyGetSetDef pyfilter_getset[] = {
    {"include", (getter) pyfilter_get_include, NULL,
     "Include or exclude the trace?"},
    {"pattern", (getter) pyfilter_get_pattern, NULL,
     "Pattern matching a filename, can contain one "
     "or many '*' joker characters"},
    {"lineno", (getter) pyfilter_get_lineno, NULL,
     "Line number"},
    {"traceback", (getter) pyfilter_get_traceback, NULL,
     "Check the whole traceback, or only the most recent frame?"},
    {NULL}
};

static PyMethodDef pyfilter_methods[] = {
    {"match", (PyCFunction)pyfilter_match,
     METH_VARARGS,
     PyDoc_STR("match(filename: str, lineno: int) -> bool")},
    {"match_traceback", (PyCFunction)pyfilter_match_traceback,
     METH_VARARGS,
     PyDoc_STR("match_traceback(traceback) -> bool")},
    {"match_filename", (PyCFunction)pyfilter_match_filename,
     METH_VARARGS,
     PyDoc_STR("match_filename(filename: str) -> bool")},
    {"match_lineno", (PyCFunction)pyfilter_match_lineno,
     METH_VARARGS,
     PyDoc_STR("match_lineno(lineno: int) -> bool")},
    {NULL,              NULL}           /* sentinel */
};

PyDoc_STRVAR(pyfilter_doc,
"Filter(include: bool, filename: str, lineno: int=None, traceback: bool=False)");

static PyTypeObject FilterType = {
    /* The ob_type field must be initialized in the module init function
     * to be portable to Windows without using C++. */
    PyVarObject_HEAD_INIT(NULL, 0)
    "tracemalloc.Filter",       /*tp_name*/
    sizeof(FilterObject),       /*tp_basicsize*/
    0,                          /*tp_itemsize*/
    /* methods */
    (destructor)pyfilter_dealloc,   /*tp_dealloc*/
    0,                          /*tp_print*/
    (getattrfunc)0,             /*tp_getattr*/
    (setattrfunc)0,             /*tp_setattr*/
    0,                          /*tp_reserved*/
    (reprfunc)pyfilter_repr,    /*tp_repr*/
    0,                          /*tp_as_number*/
    0,                          /*tp_as_sequence*/
    0,                          /*tp_as_mapping*/
    (hashfunc)pyfilter_hash,    /*tp_hash*/
    0,                          /*tp_call*/
    0,                          /*tp_str*/
    (getattrofunc)0,            /*tp_getattro*/
    0,                          /*tp_setattro*/
    0,                          /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT,         /*tp_flags*/
    pyfilter_doc,               /*tp_doc*/
    0,                          /*tp_traverse*/
    0,                          /*tp_clear*/
    (richcmpfunc)pyfilter_richcompare, /*tp_richcompare*/
    0,                          /*tp_weaklistoffset*/
    0,                          /*tp_iter*/
    0,                          /*tp_iternext*/
    pyfilter_methods,           /*tp_methods*/
    0,                          /*tp_members*/
    pyfilter_getset,            /* tp_getset */
    0,                          /*tp_base*/
    0,                          /*tp_dict*/
    0,                          /*tp_descr_get*/
    0,                          /*tp_descr_set*/
    0,                          /*tp_dictoffset*/
    (initproc)pyfilter_init,    /* tp_init */
    0,                          /*tp_alloc*/
    PyType_GenericNew,          /*tp_new*/
    0,                          /*tp_free*/
    0,                          /*tp_is_gc*/
};

typedef struct {
    PyObject_HEAD
    task_t task;
} TaskObject;

/* Forward declaration */
static PyTypeObject TaskType;

static PyObject*
pytask_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    TaskObject *op;

    op = (TaskObject *)type->tp_alloc(type, 0);
    if (op == NULL)
        return NULL;

    task_init(&op->task);
    return (PyObject *)op;
}

static int
pytask_init(TaskObject *self, PyObject *args, PyObject *kwargs)
{
    PyObject *func, *func_args;
    Py_ssize_t narg;

    narg = PyTuple_GET_SIZE(args);
    if (narg == 0) {
        PyErr_SetString(PyExc_ValueError, "missing func parameter");
        return -1;
    }

    func = PyTuple_GET_ITEM(args, 0);
    if (!PyCallable_Check(func)) {
        PyErr_Format(PyExc_TypeError,
                     "func must be a callable object, not %s",
                     Py_TYPE(func)->tp_name);
        return -1;
    }

    func_args = PyTuple_GetSlice(args, 1, narg);
    if (func_args == NULL)
        return -1;

    Py_INCREF(func);
    self->task.func = func;
    self->task.func_args = func_args;
    Py_XINCREF(kwargs);
    self->task.func_kwargs = kwargs;

    self->task.ncall = -1;
    return 0;
}

static PyObject *
pytask_is_scheduled(TaskObject *self)
{
    int scheduled = PySequence_Contains(tracemalloc_tasks, (PyObject*)self);
    if (scheduled == -1)
        return NULL;
    return PyBool_FromLong(scheduled);
}

static PyObject *
pytask_get_func(TaskObject *self, void *closure)
{
    Py_INCREF(self->task.func);
    return self->task.func;
}

static int
pytask_set_func(TaskObject *self, PyObject *func)
{
    if (!PyCallable_Check(func)) {
        PyErr_Format(PyExc_TypeError,
                     "func must be a callable object, not %s",
                     Py_TYPE(func)->tp_name);
        return -1;
    }

    Py_DECREF(self->task.func);
    Py_INCREF(func);
    self->task.func = func;
    return 0;
}

static int
pytask_set_func_args(TaskObject *self, PyObject *func_args)
{
    if (!PyTuple_Check(func_args)) {
        PyErr_Format(PyExc_TypeError,
                     "func_args must be a tuple, not %s",
                     Py_TYPE(func_args)->tp_name);
        return -1;
    }

    Py_DECREF(self->task.func_args);
    Py_INCREF(func_args);
    self->task.func_args = func_args;
    return 0;
}

static PyObject *
pytask_get_func_args(TaskObject *self, void *closure)
{
    Py_INCREF(self->task.func_args);
    return self->task.func_args;
}

static PyObject *
pytask_get_func_kwargs(TaskObject *self, void *closure)
{
    if (self->task.func_kwargs) {
        Py_INCREF(self->task.func_kwargs);
        return self->task.func_kwargs;
    }
    else
        Py_RETURN_NONE;
}

static int
pytask_set_func_kwargs(TaskObject *self, PyObject *func_kwargs)
{
    if (!PyDict_Check(func_kwargs) && func_kwargs != Py_None) {
        PyErr_Format(PyExc_TypeError,
                     "func_kwargs must be a dict or None, not %s",
                     Py_TYPE(func_kwargs)->tp_name);
        return -1;
    }

    Py_DECREF(self->task.func_kwargs);
    Py_INCREF(func_kwargs);
    self->task.func_kwargs = func_kwargs;
    return 0;
}

static int
pytask_reschedule(TaskObject *self)
{
    int scheduled;

    if (!tracemalloc_config.enabled) {
        /* a task cannot be scheduled if the tracemalloc module is disabled */
        return 0;
    }

    /* task_call_later() may have scheduled a call to the task: ensure
       that the pending call list is empty */
    if (Py_MakePendingCalls() < 0)
        return -1;

    set_reentrant(1);
    scheduled = PySequence_Contains(tracemalloc_tasks, (PyObject*)self);
    set_reentrant(0);
    if (scheduled == -1)
        return -1;

    if (scheduled == 1)
        task_reschedule(&self->task);

    return 0;
}

static Py_ssize_t
pyobj_as_ssize_t(PyObject *obj)
{
#ifndef PYTHON3
    if (PyInt_Check(obj))
        return PyInt_AsSsize_t(obj);
    else
#endif
    if (PyLong_Check(obj)) {
        return PyLong_AsSsize_t(obj);
    }
    else {
        PyErr_Format(PyExc_TypeError, "expect a long, got %s",
                     Py_TYPE(obj)->tp_name);
        return -1;
    }
}

static PyObject *
pytask_schedule(TaskObject *self, PyObject *args)
{
    PyObject *repeat_obj = Py_None;
    Py_ssize_t repeat;
    int scheduled;

    if (!PyArg_ParseTuple(args, "|O:schedule",
                          &repeat_obj))
        return NULL;

    if (repeat_obj != Py_None) {
        repeat = pyobj_as_ssize_t(repeat_obj);
        if (repeat == -1 && PyErr_Occurred())
            return NULL;
        if (repeat <= 0) {
            PyErr_SetString(PyExc_ValueError,
                            "repeat must be positive or None");
            return NULL;
        }
    }
    else
        repeat = -1;

    if (!tracemalloc_config.enabled) {
        PyErr_SetString(PyExc_RuntimeError,
                        "the tracemalloc module must be enabled "
                        "to schedule a task");
        return NULL;
    }

    if (self->task.delay <= 0 && self->task.memory_threshold <= 0) {
        PyErr_SetString(PyExc_ValueError,
                        "delay and memory_threshold are None");
        return NULL;
    }

    scheduled = PySequence_Contains(tracemalloc_tasks, (PyObject*)self);
    if (scheduled == -1)
        return NULL;
    if (!scheduled) {
        if (PyList_Append(tracemalloc_tasks, (PyObject *)self) < 0)
            return NULL;

        self->task.ncall = repeat;
        task_schedule(&self->task);
    }
    else {
        if (pytask_reschedule(self) < 0)
            return NULL;
    }

    Py_RETURN_NONE;
}

static PyObject *
pytask_cancel(TaskObject *self)
{
    PyObject *res;
#ifdef PEP393
    _Py_IDENTIFIER(remove);

    res = _PyObject_CallMethodId(tracemalloc_tasks, &PyId_remove, "O", self);
#else
    res = PyObject_CallMethod(tracemalloc_tasks, "remove", "O", self);
#endif
    if (res == NULL) {
        if (!PyErr_ExceptionMatches(PyExc_ValueError))
            return NULL;
        /* task not scheduled: ignore the error */
        PyErr_Clear();
    }
    else
        Py_DECREF(res);

    task_cancel(&self->task);

    Py_RETURN_NONE;
}

static PyObject *
pytask_call(TaskObject *self)
{
    return task_call(&self->task);
}

static PyObject *
pytask_get_delay(TaskObject *self)
{
    if (self->task.delay > 0)
        return PyLong_FromLong(self->task.delay);
    else
        Py_RETURN_NONE;
}

static PyObject*
pytask_set_delay(TaskObject *self, PyObject *delay_obj)
{
    int delay;

    if (delay_obj != Py_None) {
        delay = _PyLong_AsInt(delay_obj);
        if (delay == -1 && PyErr_Occurred())
            return NULL;
        if (delay <= 0) {
            PyErr_SetString(PyExc_ValueError,
                            "delay must be positive or None");
            return NULL;
        }
    }
    else
        delay = -1;

    self->task.delay = delay;
    if (pytask_reschedule(self) < 0)
        return NULL;

    Py_RETURN_NONE;
}

static PyObject *
pytask_get_memory_threshold(TaskObject *self)
{
    if (self->task.memory_threshold > 0)
        return PyLong_FromSsize_t(self->task.memory_threshold);
    else
        Py_RETURN_NONE;
}

static PyObject*
pytask_set_memory_threshold(TaskObject *self, PyObject *threshold_obj)
{
    Py_ssize_t memory_threshold;

    if (threshold_obj != Py_None) {
        memory_threshold = pyobj_as_ssize_t(threshold_obj);
        if (memory_threshold == -1 && PyErr_Occurred())
            return NULL;
        if (memory_threshold <= 0) {
            PyErr_SetString(PyExc_ValueError,
                            "size must be greater than zero "
                            "or None");
            return NULL;
        }
    }
    else
        memory_threshold = -1;

    self->task.memory_threshold = memory_threshold;
    if (pytask_reschedule(self) < 0)
        return NULL;

    Py_RETURN_NONE;
}

static PyObject*
pytask_repr(TaskObject *self)
{
    return PyUnicode_FromFormat("<tracemalloc.Task %R>",
                                self->task.func);
}

static void
pytask_dealloc(TaskObject *self)
{
    assert(tracemalloc_tasks == NULL
           || PySequence_Contains(tracemalloc_tasks, (PyObject*)self) == 0);

    Py_CLEAR(self->task.func);
    Py_CLEAR(self->task.func_args);
    Py_CLEAR(self->task.func_kwargs);

    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyGetSetDef tracemalloc_pytask_getset[] = {
    {"func",
     (getter)pytask_get_func, (setter)pytask_set_func,
     "Function"},
    {"func_args",
     (getter)pytask_get_func_args, (setter)pytask_set_func_args,
     "Function arguments"},
    {"func_kwargs",
     (getter)pytask_get_func_kwargs, (setter)pytask_set_func_kwargs,
     "Function keyword arguments"},
    {NULL}
};

static PyMethodDef tracemalloc_pytask_methods[] = {
    {"call", (PyCFunction)pytask_call,
     METH_NOARGS,
     PyDoc_STR("call()")},
    {"is_scheduled", (PyCFunction)pytask_is_scheduled,
     METH_NOARGS,
     PyDoc_STR("is_enabled()")},
    {"schedule", (PyCFunction)pytask_schedule,
     METH_VARARGS,
     PyDoc_STR("schedule(repeat: int=None)")},
    {"cancel", (PyCFunction)pytask_cancel,
     METH_NOARGS,
     PyDoc_STR("cancel()")},
    {"get_delay", (PyCFunction)pytask_get_delay,
     METH_NOARGS,
     PyDoc_STR("get_delay()->int|None")},
    {"set_delay", (PyCFunction)pytask_set_delay,
     METH_O,
     PyDoc_STR("set_delay(seconds: int)")},
    {"get_memory_threshold", (PyCFunction)pytask_get_memory_threshold,
     METH_NOARGS,
     PyDoc_STR("get_memory_threshold()->int|None")},
    {"set_memory_threshold", (PyCFunction)pytask_set_memory_threshold,
     METH_O,
     PyDoc_STR("set_memory_threshold(size: int)")},
    {NULL,              NULL}           /* sentinel */
};

PyDoc_STRVAR(tracemalloc_pytask_doc,
"Task(func, *args, **kw)");

static PyTypeObject TaskType = {
    /* The ob_type field must be initialized in the module init function
     * to be portable to Windows without using C++. */
    PyVarObject_HEAD_INIT(NULL, 0)
    "tracemalloc.Task",         /*tp_name*/
    sizeof(TaskObject),         /*tp_basicsize*/
    0,                          /*tp_itemsize*/
    /* methods */
    (destructor)pytask_dealloc, /*tp_dealloc*/
    0,                          /*tp_print*/
    0,                          /*tp_getattr*/
    0,                          /*tp_setattr*/
    0,                          /*tp_reserved*/
    (reprfunc)pytask_repr,      /*tp_repr*/
    0,                          /*tp_as_number*/
    0,                          /*tp_as_sequence*/
    0,                          /*tp_as_mapping*/
    0,                          /*tp_hash*/
    0,                          /*tp_call*/
    0,                          /*tp_str*/
    0,                          /*tp_getattro*/
    0,                          /*tp_setattro*/
    0,                          /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /*tp_flags*/
    tracemalloc_pytask_doc,     /*tp_doc*/
    0,                          /*tp_traverse*/
    0,                          /*tp_clear*/
    0,                          /*tp_richcompare*/
    0,                          /*tp_weaklistoffset*/
    0,                          /*tp_iter*/
    0,                          /*tp_iternext*/
    tracemalloc_pytask_methods, /*tp_methods*/
    0,                          /*tp_members*/
    tracemalloc_pytask_getset,  /*tp_getset*/
    0,                          /*tp_base*/
    0,                          /*tp_dict*/
    0,                          /*tp_descr_get*/
    0,                          /*tp_descr_set*/
    0,                          /*tp_dictoffset*/
    (initproc)pytask_init,      /*tp_init*/
    PyType_GenericAlloc,        /*tp_alloc*/
    pytask_new,                 /*tp_new*/
    PyObject_Del,               /*tp_free*/
    0,                          /*tp_is_gc*/
};

static void
task_list_check(void)
{
    Py_ssize_t i, len;
    TaskObject *obj;
    int res, reset_reentrant;

    len = PyList_GET_SIZE(tracemalloc_tasks);
    for (i=0; i<len; i++) {
        obj = (TaskObject *)PyList_GET_ITEM(tracemalloc_tasks, i);
        if (obj->task.enabled)
            task_check(&obj->task);
    }

    reset_reentrant = 0;
    for (i=len-1; i>=0; i--) {
        obj = (TaskObject *)PyList_GET_ITEM(tracemalloc_tasks, i);
        if (!obj->task.failed)
            continue;

        if (!reset_reentrant) {
            /* the list may be reallocated to be shrinked */
            set_reentrant(1);
            reset_reentrant = 1;
        }

        res = PyList_SetSlice(tracemalloc_tasks, i, i+1, NULL);
        if (res < 0) {
            PyErr_WriteUnraisable(NULL);
            break;
        }
    }
    if (reset_reentrant)
        set_reentrant(0);
}

static int
task_list_clear(void)
{
    Py_ssize_t len;
    int res;

    /* task_call_later() may have scheduled calls to a task which will be
       destroyed: ensure that the pending call list is empty */
    if (Py_MakePendingCalls() < 0)
        return -1;

    len = PyList_GET_SIZE(tracemalloc_tasks);

    set_reentrant(1);
    res = PyList_SetSlice(tracemalloc_tasks, 0, len, NULL);
    set_reentrant(0);

    return res;
}

static PyObject*
py_tracemalloc_is_enabled(PyObject *self)
{
    return PyBool_FromLong(tracemalloc_config.enabled);
}

PyDoc_STRVAR(tracemalloc_clear_traces_doc,
    "clear_traces()\n"
    "\n"
    "Clear all traces and statistics of memory allocations.");

static PyObject*
py_tracemalloc_clear_traces(PyObject *self)
{
    if (!tracemalloc_config.enabled)
        Py_RETURN_NONE;

    set_reentrant(1);
    tracemalloc_clear_traces();
    set_reentrant(0);

    Py_RETURN_NONE;
}

PyDoc_STRVAR(tracemalloc_get_stats_doc,
"get_stats() -> dict\n"
"\n"
"Get statistics on Python memory allocations per Python filename and per\n"
"line number.\n"
"\n"
"Return a dictionary {filename: str -> {line_number: int -> stats}}\n"
"where stats in a (size: int, count: int) tuple.\n"
"\n"
"Return an empty dictionary if the module tracemalloc is disabled.");

static PyObject*
tracemalloc_get_stats(PyObject *self)
{
    get_stats_t get_stats;
    int err;

    if (!tracemalloc_config.enabled)
        return PyDict_New();

    pool_init(&get_stats.pool, tracemalloc_file_stats->pool->item_size);
    get_stats.file_dict = PyDict_New();
    if (get_stats.file_dict == NULL)
        goto error;

    TABLES_LOCK();
    get_stats.file_stats = hash_copy_with_pool(tracemalloc_file_stats,
                                               &get_stats.pool);
    TABLES_UNLOCK();

    if (get_stats.file_stats == NULL) {
        PyErr_NoMemory();
        goto error;
    }

    set_reentrant(1);
    err = hash_foreach(get_stats.file_stats,
                       tracemalloc_get_stats_fill_file, &get_stats);
    set_reentrant(0);
    if (err) {
        goto error;
    }

    goto finally;

error:
    Py_CLEAR(get_stats.file_dict);

finally:
    if (get_stats.file_stats != NULL)
        hash_destroy(get_stats.file_stats);
#ifdef USE_MEMORY_POOL
    pool_clear(&get_stats.pool);
#endif
    return get_stats.file_dict;
}

static PyObject*
frame_to_pyobject(frame_t *frame)
{
    PyObject *frame_obj, *lineno_obj;

    frame_obj = PyTuple_New(2);
    if (frame_obj == NULL)
        return NULL;

    if (frame->filename == NULL)
        frame->filename = Py_None;
    Py_INCREF(frame->filename);
    PyTuple_SET_ITEM(frame_obj, 0, frame->filename);

    lineno_obj = tracemalloc_lineno_as_obj(frame->lineno);
    if (lineno_obj == NULL) {
        Py_DECREF(frame_obj);
        return NULL;
    }
    PyTuple_SET_ITEM(frame_obj, 1, lineno_obj);

    return frame_obj;
}

static PyObject*
traceback_to_pyobject(traceback_t *traceback, hash_t *intern_table)
{
    int i;
    PyObject *frames, *frame;

    if (intern_table != NULL) {
        if (HASH_GET_DATA(intern_table, traceback, frames)) {
            Py_INCREF(frames);
            return frames;
        }
    }

    frames = PyTuple_New(traceback->nframe);
    if (frames == NULL)
        return NULL;

    for (i=0; i < traceback->nframe; i++) {
        frame = frame_to_pyobject(&traceback->frames[i]);
        if (frame == NULL) {
            Py_DECREF(frames);
            return NULL;
        }
        PyTuple_SET_ITEM(frames, i, frame);
    }

    if (intern_table != NULL) {
        if (HASH_PUT_DATA(intern_table, traceback, frames) < 0) {
            Py_DECREF(frames);
            PyErr_NoMemory();
            return NULL;
        }
        /* intern_table keeps a new reference to frames */
        Py_INCREF(frames);
    }
    return frames;
}

static PyObject*
trace_to_pyobject(trace_t *trace, hash_t *intern_tracebacks)
{
    PyObject *trace_obj = NULL;
    PyObject *size, *traceback;

    trace_obj = PyTuple_New(2);
    if (trace_obj == NULL)
        return NULL;

    size = PyLong_FromSize_t(trace->size);
    if (size == NULL) {
        Py_DECREF(trace_obj);
        return NULL;
    }
    PyTuple_SET_ITEM(trace_obj, 0, size);

    traceback = traceback_to_pyobject(trace->traceback, intern_tracebacks);
    if (traceback == NULL) {
        Py_DECREF(trace_obj);
        return NULL;
    }
    PyTuple_SET_ITEM(trace_obj, 1, traceback);

    return trace_obj;
}

typedef struct {
    pool_t tracebacks_pool;
    pool_t traces_pool;
    hash_t *traces;
    hash_t *tracebacks;
    PyObject *dict;
} get_traces_t;

static int
tracemalloc_get_traces_fill(hash_entry_t *entry, void *user_data)
{
    get_traces_t *get_traces = user_data;
    const void *ptr;
    trace_t *trace;
    PyObject *key_obj, *tracemalloc_obj;
    int res;

    ptr = entry->key;
    trace = (trace_t *)ENTRY_DATA_PTR(entry);

    key_obj = PyLong_FromVoidPtr((void *)ptr);
    if (key_obj == NULL)
        return 1;

    tracemalloc_obj = trace_to_pyobject(trace, get_traces->tracebacks);
    if (tracemalloc_obj == NULL) {
        Py_DECREF(key_obj);
        return 1;
    }

    res = PyDict_SetItem(get_traces->dict, key_obj, tracemalloc_obj);
    Py_DECREF(key_obj);
    Py_DECREF(tracemalloc_obj);
    if (res < 0)
        return 1;

    return 0;
}

static int
tracemalloc_pyobject_decref_cb(hash_entry_t *entry, void *user_data)
{
    PyObject *obj = (PyObject *)HASH_ENTRY_DATA_AS_VOID_P(entry);
    Py_DECREF(obj);
    return 0;
}

PyDoc_STRVAR(tracemalloc_get_traces_doc,
"get_stats() -> dict\n"
"\n"
"Get all traces of allocated Python memory blocks.\n"
"Return a dictionary: {pointer: int -> trace: structseq).\n"
"Return an empty dictionary if the tracemalloc module is disabled.");

static PyObject*
py_tracemalloc_get_traces(PyObject *self, PyObject *obj)
{
    get_traces_t get_traces;
    int err;

    if (!tracemalloc_config.enabled)
        return PyDict_New();

    pool_init(&get_traces.tracebacks_pool,
              sizeof(hash_entry_t) + sizeof(PyObject *));
    pool_init(&get_traces.traces_pool, tracemalloc_traces->pool->item_size);
    get_traces.traces = NULL;
    get_traces.tracebacks = NULL;
    get_traces.dict = PyDict_New();
    if (get_traces.dict == NULL)
        goto error;

    get_traces.tracebacks = hash_new(&get_traces.tracebacks_pool,
                                     sizeof(PyObject *),
                                     key_hash_int, key_cmp_direct);
    if (get_traces.tracebacks == NULL) {
        PyErr_NoMemory();
        goto error;
    }

    TABLES_LOCK();
    /* allocate the exact number of traces */
    if (pool_alloc(&get_traces.traces_pool, tracemalloc_traces->entries) < 0)
        get_traces.traces = NULL;
    else
        get_traces.traces = hash_copy_with_pool(tracemalloc_traces,
                                                &get_traces.traces_pool);
    TABLES_UNLOCK();

    if (get_traces.traces == NULL) {
        PyErr_NoMemory();
        goto error;
    }

    set_reentrant(1);
    err = hash_foreach(get_traces.traces,
                       tracemalloc_get_traces_fill, &get_traces);
    set_reentrant(0);
    if (err)
        goto error;

    goto finally;

error:
    Py_CLEAR(get_traces.dict);

finally:
    if (get_traces.tracebacks != NULL) {
        hash_foreach(get_traces.tracebacks,
                     tracemalloc_pyobject_decref_cb, NULL);
        hash_destroy(get_traces.tracebacks);
    }
    if (get_traces.traces != NULL)
        hash_destroy(get_traces.traces);
#ifdef USE_MEMORY_POOL
    pool_clear(&get_traces.traces_pool);
    pool_clear(&get_traces.tracebacks_pool);
#endif
    return get_traces.dict;
}

void*
tracemalloc_get_object_address(PyObject *obj)
{
    PyTypeObject *type = Py_TYPE(obj);
    if (PyType_IS_GC(type))
        return (void *)((char *)obj - sizeof(PyGC_Head));
    else
        return (void *)obj;
}

PyDoc_STRVAR(tracemalloc_get_object_address_doc,
"get_object_address(obj) -> int\n"
"\n"
"Return the address of the memory block of the specified\n"
"Python object.");

static PyObject*
py_tracemalloc_get_object_address(PyObject *self, PyObject *obj)
{
    void *ptr = tracemalloc_get_object_address(obj);
    return PyLong_FromVoidPtr(ptr);
}

static PyObject*
tracemalloc_get_trace(void *ptr)
{
    trace_t trace;
    int found;

    if (!tracemalloc_config.enabled)
        Py_RETURN_NONE;

    TABLES_LOCK();
    found = HASH_GET_DATA(tracemalloc_traces, ptr, trace);
    TABLES_UNLOCK();

    if (found)
        return trace_to_pyobject(&trace, NULL);
    else
        Py_RETURN_NONE;
}

PyDoc_STRVAR(tracemalloc_get_trace_doc,
"get_trace(address) -> trace\n"
"\n"
"Get the trace of the Python memory block allocated at specified address.");

static PyObject*
py_tracemalloc_get_trace(PyObject *self, PyObject *obj)
{
    void *ptr = PyLong_AsVoidPtr(obj);
    if (ptr == NULL && PyErr_Occurred())
        return NULL;

    return tracemalloc_get_trace(ptr);
}

PyDoc_STRVAR(tracemalloc_get_object_trace_doc,
"get_object_trace(obj) -> trace\n"
"\n"
"Get the trace of the Python object 'obj' as trace structseq.\n"
"Return None if tracemalloc module did not save the location\n"
"when the object was allocated, for example if tracemalloc was disabled.");

static PyObject*
py_tracemalloc_get_object_trace(PyObject *self, PyObject *obj)
{
    void *ptr;

    ptr = tracemalloc_get_object_address(obj);

    return tracemalloc_get_trace(ptr);
}

static int
tracemalloc_atexit_register(PyObject *module)
{
    PyObject *method = NULL, *atexit = NULL, *func = NULL;
    PyObject *result;
    int ret = -1;

    method = PyObject_GetAttrString(module, "_atexit");
    if (method == NULL)
        goto done;

    atexit = PyImport_ImportModule("atexit");
    if (atexit == NULL) {
        if (!PyErr_Warn(PyExc_ImportWarning,
                       "atexit module is missing: "
                       "cannot automatically disable tracemalloc at exit"))
        {
            PyErr_Clear();
            return 0;
        }
        goto done;
    }

    func = PyObject_GetAttrString(atexit, "register");
    if (func == NULL)
        goto done;

    result = PyObject_CallFunction(func, "O", method);
    if (result == NULL)
        goto done;
    Py_DECREF(result);

    ret = 0;

done:
    Py_XDECREF(method);
    Py_XDECREF(func);
    Py_XDECREF(atexit);
    return ret;
}

PyDoc_STRVAR(tracemalloc_cancel_tasks_doc,
    "cancel_tasks()\n"
    "\n"
    "Stop scheduled tasks.");

PyObject*
tracemalloc_cancel_tasks(PyObject *module)
{
    int res;

    res = task_list_clear();
    if (res < 0)
        return NULL;

    Py_RETURN_NONE;
}

PyDoc_STRVAR(tracemalloc_get_tasks_doc,
    "get_tasks()\n"
    "\n"
    "Get the list of scheduled tasks.");

PyObject*
tracemalloc_get_tasks(PyObject *module)
{
    PyObject *list;

    list = PyList_New(0);
    if (list == NULL)
        return NULL;

    if (_PyList_Extend((PyListObject*)list, tracemalloc_tasks) < 0) {
        Py_DECREF(list);
        return NULL;
    }
    return list;
}

PyDoc_STRVAR(tracemalloc_enable_doc,
    "enable()\n"
    "\n"
    "Start tracing Python memory allocations.");

static PyObject*
py_tracemalloc_enable(PyObject *self)
{
    if (tracemalloc_enable() < 0)
        return NULL;

    Py_RETURN_NONE;
}

PyDoc_STRVAR(tracemalloc_disable_doc,
    "disable()\n"
    "\n"
    "Stop tracing Python memory allocations.");

static PyObject*
py_tracemalloc_disable(PyObject *self)
{
    if (tracemalloc_disable() < 0)
        return NULL;

    Py_RETURN_NONE;
}

static PyObject*
tracemalloc_atexit(PyObject *self)
{
    assert(PyGILState_Check());
    tracemalloc_deinit();
    Py_RETURN_NONE;
}

PyDoc_STRVAR(tracemalloc_get_traceback_limit_doc,
    "get_traceback_limit() -> int\n"
    "\n"
    "Get the maximum number of frames stored in a trace of a memory\n"
    "allocation.");

static PyObject*
py_tracemalloc_get_traceback_limit(PyObject *self)
{
    return PyLong_FromLong(tracemalloc_config.max_nframe);
}

PyDoc_STRVAR(tracemalloc_set_traceback_limit_doc,
    "set_traceback_limit(nframe: int)\n"
    "\n"
    "Set the maximum number of frames stored in the traceback attribute\n"
    "of a trace of a memory allocation.\n"
    "\n"
    "If the tracemalloc is enabled, all traces and statistics of memory\n"
    "allocations are cleared.");

static PyObject*
tracemalloc_set_traceback_limit(PyObject *self, PyObject *args)
{
    int nframe;

    if (!PyArg_ParseTuple(args, "i:set_traceback_limit",
                          &nframe))
        return NULL;

    if (nframe < 0 || nframe > MAX_NFRAME) {
        PyErr_Format(PyExc_ValueError,
                     "the number of frames must be in range [0; %i]",
                     MAX_NFRAME);
        return NULL;
    }
    tracemalloc_config.max_nframe = nframe;

    Py_RETURN_NONE;
}

PyDoc_STRVAR(tracemalloc_get_tracemalloc_memory_doc,
    "get_tracemalloc_memory() -> int\n"
    "\n"
    "Get the memory usage in bytes of the _tracemalloc module.");

static PyObject*
tracemalloc_get_tracemalloc_memory(PyObject *self)
{
    mem_stats_t stats;
    PyObject *size_obj, *free_obj;

    memset(&stats, 0, sizeof(stats));

#ifdef PRINT_STATS
    hash_print_stats(tracemalloc_filenames);
    hash_print_stats(tracemalloc_tracebacks);
#ifdef TRACE_ARENA
    hash_print_stats(tracemalloc_arenas);
#endif
    TABLES_LOCK();
    hash_print_stats(tracemalloc_traces);
    hash_print_stats(tracemalloc_file_stats);
    TABLES_UNLOCK();

    pool_print_stats(&tracemalloc_pools.no_data);
    TABLES_LOCK();
    pool_print_stats(&tracemalloc_pools.traces);
    pool_print_stats(&tracemalloc_pools.hash_tables);
    TABLES_UNLOCK();
#endif

    /* hash tables */
    hash_mem_stats(tracemalloc_tracebacks, &stats);
    hash_mem_stats(tracemalloc_filenames, &stats);
#ifdef TRACE_ARENA
    hash_mem_stats(tracemalloc_arenas, &stats);
#endif

    TABLES_LOCK();
    hash_mem_stats(tracemalloc_traces, &stats);
    hash_mem_stats(tracemalloc_file_stats, &stats);
    TABLES_UNLOCK();

    /* memory pools */
    pool_mem_stats(&tracemalloc_pools.no_data, &stats);
    pool_mem_stats(&tracemalloc_pools.traces, &stats);
    pool_mem_stats(&tracemalloc_pools.hash_tables, &stats);

    size_obj = PyLong_FromSize_t(stats.data + stats.free);
    free_obj = PyLong_FromSize_t(stats.free);
    return Py_BuildValue("NN", size_obj, free_obj);
}

PyDoc_STRVAR(tracemalloc_get_traced_memory_doc,
    "get_traced_memory() -> int\n"
    "\n"
    "Get the total size in bytes of all memory blocks allocated\n"
    "by Python currently.");

static PyObject*
tracemalloc_get_traced_memory(PyObject *self)
{
    size_t size, max_size;
    PyObject *size_obj, *max_size_obj;

    TABLES_LOCK();
    size = tracemalloc_traced_memory;
    max_size = tracemalloc_max_traced_memory;
    TABLES_UNLOCK();

    size_obj = PyLong_FromSize_t(size);
    max_size_obj = PyLong_FromSize_t(max_size);
    return Py_BuildValue("NN", size_obj, max_size_obj);
}

#ifdef TRACE_ARENA
PyDoc_STRVAR(tracemalloc_get_arena_size_doc,
    "get_arena_size() -> int\n"
    "\n"
    "Get the total size in bytes of all arenas.");

static PyObject*
tracemalloc_get_arena_size(PyObject *self)
{
    return PyLong_FromSize_t(tracemalloc_arena_size);
}
#endif

static int
tracemalloc_add_filter(filter_t *filter)
{
    size_t i, nfilter;
    filter_list_t *filters;
    filter_t *new_filters;

    if (filter->include)
        filters = &tracemalloc_include_filters;
    else
        filters = &tracemalloc_exclude_filters;

    for(i=0; i<filters->nfilter; i++) {
        if (filter_compare(&filters->filters[i], filter) == 1) {
            /* filter already present, don't add a duplicate */
            return 0;
        }
    }

    nfilter = (filters->nfilter + 1);
    new_filters = raw_realloc(filters->filters, nfilter * sizeof(filter_t));
    if (new_filters == NULL) {
        PyErr_NoMemory();
        return -1;
    }

    Py_INCREF(filter->pattern);
    new_filters[filters->nfilter] = *filter;

    filters->nfilter = nfilter;
    filters->filters = new_filters;
    return 0;
}

PyDoc_STRVAR(tracemalloc_add_filter_doc,
    "add_filter(include: bool, filename: str, lineno: int=None, traceback: bool=True)\n"
    "\n"
    "Add a filter. If include is True, only trace memory blocks allocated\n"
    "in a file with a name matching filename at line number lineno. If\n"
    "include is True, don't trace memory blocks allocated in a file with a\n"
    "name matching filename at line number lineno.\n"
    "\n"
    "The filename can contain one or many '*' joker characters which\n"
    "matchs any substring, including an empty string. The '.pyc' and '.pyo'\n"
    "suffixes are automatically replaced with '.py'. On Windows, the\n"
    "comparison is case insensitive and the alternative separator '/' is\n"
    "replaced with the standard separator '\'.\n"
    "\n"
    "If lineno is None or lesser than 1, it matches any line number.");

static PyObject*
py_tracemalloc_add_filter(PyObject *self, PyObject *args)
{
    FilterObject *pyfilter;

    if (!PyArg_ParseTuple(args, "O!:add_filter",
                          &FilterType, (PyObject **)&pyfilter))
        return NULL;

    if (tracemalloc_add_filter(&pyfilter->filter) < 0)
        return NULL;

    Py_RETURN_NONE;
}

PyDoc_STRVAR(tracemalloc_add_include_filter_doc,
    "add_include_filter(filename: str, lineno: int=None, traceback: bool=False)");

static PyObject*
tracemalloc_add_include_filter(PyObject *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"filename", "lineno", "traceback", 0};
    PyObject *filename;
    int lineno = -1;
    int traceback = 0;
    filter_t filter;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|O&i:add_include_filter", kwlist,
                                     &filename,
                                     &tracemalloc_parse_lineno, &lineno,
                                     &traceback))
        return NULL;

    if (filter_init(&filter, 1, filename, lineno, traceback) < 0)
        return NULL;

    if (tracemalloc_add_filter(&filter) < 0) {
        filter_deinit(&filter);
        return NULL;
    }
    filter_deinit(&filter);

    Py_RETURN_NONE;
}

PyDoc_STRVAR(tracemalloc_add_exclude_filter_doc,
    "add_exclude_filter(filename: str, lineno: int=None, traceback: bool=False)");

static PyObject*
tracemalloc_add_exclude_filter(PyObject *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"filename", "lineno", "traceback", 0};
    PyObject *filename;
    int lineno = -1;
    int traceback = 0;
    filter_t filter;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|O&i:add_exclude_filter", kwlist,
                                     &filename,
                                     &tracemalloc_parse_lineno, &lineno,
                                     &traceback))
        return NULL;

    if (filter_init(&filter, 0, filename, lineno, traceback) < 0)
        return NULL;

    if (tracemalloc_add_filter(&filter) < 0) {
        filter_deinit(&filter);
        return NULL;
    }
    filter_deinit(&filter);

    Py_RETURN_NONE;
}

static PyObject*
filter_as_obj(filter_t *filter)
{
    FilterObject *pyfilter;

    pyfilter = PyObject_New(FilterObject, &FilterType);
    if (pyfilter == NULL)
        return NULL;

    Py_INCREF(filter->pattern);
    pyfilter->filter = *filter;
    return (PyObject *)pyfilter;
}

PyDoc_STRVAR(tracemalloc_get_filters_doc,
    "get_filters()\n"
    "\n"
    "Get the filters as list of (include: bool, filename: str, lineno: int)"
    "tuples.\n"
    "\n"
    "If *lineno* is ``None``, a filter matchs any line number.");

static size_t
tracemalloc_get_filters(PyObject *list, size_t first_index,
                        filter_list_t *filters)
{
    size_t i;
    filter_t *filter;
    PyObject *pyfilter;

    for (i=0; i<filters->nfilter; i++) {
        filter = &filters->filters[i];

        pyfilter = filter_as_obj(filter);
        if (pyfilter == NULL)
            return (size_t)-1;

        PyList_SET_ITEM(list, first_index + i, pyfilter);
    }
    return filters->nfilter;
}

static PyObject*
py_tracemalloc_get_filters(PyObject *self)
{
    PyObject *filters = NULL;
    size_t number;

    filters = PyList_New(tracemalloc_include_filters.nfilter
                         + tracemalloc_exclude_filters.nfilter);
    if (filters == NULL)
        return NULL;

    number = tracemalloc_get_filters(filters, 0, &tracemalloc_include_filters);
    if (number == (size_t)-1) {
        Py_DECREF(filters);
        return NULL;
    }

    number = tracemalloc_get_filters(filters, number, &tracemalloc_exclude_filters);
    if (number == (size_t)-1) {
        Py_DECREF(filters);
        return NULL;
    }

    return filters;
}

PyDoc_STRVAR(tracemalloc_clear_filters_doc,
    "clear_filters()\n"
    "\n"
    "Reset the filter list.");

static PyObject*
py_tracemalloc_clear_filters(PyObject *self)
{
    tracemalloc_clear_filters();
    Py_RETURN_NONE;
}

#ifdef MS_WINDOWS
PyDoc_STRVAR(tracemalloc_get_process_memory_doc,
    "get_process_memory()\n"
    "\n"
    "Get the memory usage of the current process as a tuple:\n"
    "(rss: int, vms: int).");

static PyObject*
tracemalloc_get_process_memory(PyObject *self)
{
    typedef BOOL (_stdcall *GPMI_PROC) (HANDLE Process, PPROCESS_MEMORY_COUNTERS ppsmemCounters, DWORD cb);
    static HINSTANCE psapi = NULL;
    static GPMI_PROC GetProcessMemoryInfo = NULL;
    HANDLE hProcess;
    PROCESS_MEMORY_COUNTERS counters;
    PyObject *rss, *vms;

    if (psapi == NULL) {
        psapi = LoadLibraryW(L"psapi.dll");
        if (psapi == NULL)
            return PyErr_SetFromWindowsErr(0);
    }

    if (GetProcessMemoryInfo == NULL) {
        GetProcessMemoryInfo = (GPMI_PROC)GetProcAddress(psapi, "GetProcessMemoryInfo");
        if (GetProcessMemoryInfo == NULL)
            return PyErr_SetFromWindowsErr(0);
    }

    hProcess = GetCurrentProcess();

    if (!GetProcessMemoryInfo(hProcess, &counters, sizeof(counters)))
        return PyErr_SetFromWindowsErr(0);

    rss = PyLong_FromSize_t(counters.WorkingSetSize);
    if (rss == NULL)
        return NULL;
    vms = PyLong_FromSize_t(counters.PagefileUsage);
    if (vms == NULL) {
        Py_DECREF(vms);
        return NULL;
    }
    return Py_BuildValue("NN", rss, vms);
}
#endif

static PyMethodDef module_methods[] = {
    {"is_enabled", (PyCFunction)py_tracemalloc_is_enabled, METH_NOARGS,
     PyDoc_STR("is_enabled()->bool")},
    {"clear_traces", (PyCFunction)py_tracemalloc_clear_traces, METH_NOARGS,
     tracemalloc_clear_traces_doc},
    {"get_stats", (PyCFunction)tracemalloc_get_stats, METH_NOARGS,
     tracemalloc_get_stats_doc},
    {"get_traces", (PyCFunction)py_tracemalloc_get_traces, METH_NOARGS,
     tracemalloc_get_traces_doc},
    {"get_object_address", (PyCFunction)py_tracemalloc_get_object_address, METH_O,
     tracemalloc_get_object_address_doc},
    {"get_object_trace", (PyCFunction)py_tracemalloc_get_object_trace, METH_O,
     tracemalloc_get_object_trace_doc},
    {"get_trace", (PyCFunction)py_tracemalloc_get_trace, METH_O,
     tracemalloc_get_trace_doc},
    {"cancel_tasks", (PyCFunction)tracemalloc_cancel_tasks, METH_NOARGS,
     tracemalloc_cancel_tasks_doc},
    {"get_tasks", (PyCFunction)tracemalloc_get_tasks, METH_NOARGS,
     tracemalloc_get_tasks_doc},
    {"enable", (PyCFunction)py_tracemalloc_enable, METH_NOARGS,
     tracemalloc_enable_doc},
    {"disable", (PyCFunction)py_tracemalloc_disable, METH_NOARGS,
     tracemalloc_disable_doc},
    {"get_traceback_limit", (PyCFunction)py_tracemalloc_get_traceback_limit,
     METH_NOARGS, tracemalloc_get_traceback_limit_doc},
    {"set_traceback_limit", (PyCFunction)tracemalloc_set_traceback_limit,
     METH_VARARGS, tracemalloc_set_traceback_limit_doc},
    {"get_tracemalloc_memory", (PyCFunction)tracemalloc_get_tracemalloc_memory,
     METH_NOARGS, tracemalloc_get_tracemalloc_memory_doc},
    {"get_traced_memory", (PyCFunction)tracemalloc_get_traced_memory,
     METH_NOARGS, tracemalloc_get_traced_memory_doc},
#ifdef TRACE_ARENA
    {"get_arena_size", (PyCFunction)tracemalloc_get_arena_size,
     METH_NOARGS, tracemalloc_get_arena_size_doc},
#endif
    {"add_filter", (PyCFunction)py_tracemalloc_add_filter,
     METH_VARARGS, tracemalloc_add_filter_doc},
    {"add_include_filter", (PyCFunction)tracemalloc_add_include_filter,
     METH_VARARGS | METH_KEYWORDS, tracemalloc_add_include_filter_doc},
    {"add_exclude_filter", (PyCFunction)tracemalloc_add_exclude_filter,
     METH_VARARGS | METH_KEYWORDS, tracemalloc_add_exclude_filter_doc},
    {"get_filters", (PyCFunction)py_tracemalloc_get_filters, METH_NOARGS,
     tracemalloc_get_filters_doc},
    {"clear_filters", (PyCFunction)py_tracemalloc_clear_filters, METH_NOARGS,
     tracemalloc_clear_filters_doc},
#ifdef MS_WINDOWS
    {"get_process_memory", (PyCFunction)tracemalloc_get_process_memory, METH_NOARGS,
     tracemalloc_get_process_memory_doc},
#endif

    /* private functions */
    {"_atexit", (PyCFunction)tracemalloc_atexit, METH_NOARGS},

    /* sentinel */
    {NULL, NULL}
};

PyDoc_STRVAR(module_doc,
"_tracemalloc module.");

#ifdef PYTHON3
static struct PyModuleDef module_def = {
    PyModuleDef_HEAD_INIT,
    "_tracemalloc",
    module_doc,
    0, /* non-negative size to be able to unload the module */
    module_methods,
    NULL,
};
#endif

PyMODINIT_FUNC
#ifdef PYTHON3
PyInit__tracemalloc(void)
#else
init_tracemalloc(void)
#endif
{
    PyObject *m, *version;

#ifdef PYTHON3
    m = PyModule_Create(&module_def);
#else
    m = Py_InitModule3("_tracemalloc", module_methods, module_doc);
#endif
    if (m == NULL)
        goto error;

    if (tracemalloc_init() < 0)
        goto error;

    if (PyType_Ready(&FilterType) < 0)
        goto error;
    if (PyType_Ready(&TaskType) < 0)
        goto error;

    Py_INCREF((PyObject*) &FilterType);
    PyModule_AddObject(m, "Filter", (PyObject*)&FilterType);
    Py_INCREF((PyObject*) &TaskType);
    PyModule_AddObject(m, "Task", (PyObject*)&TaskType);

#ifdef PYTHON3
    version = PyUnicode_FromString(VERSION);
#else
    version = PyString_FromString(VERSION);
#endif
    if (version == NULL)
        goto error;
    PyModule_AddObject(m, "__version__", version);

    if (tracemalloc_atexit_register(m) < 0)
        goto error;
#ifdef PYTHON3
    return m;
#else
    return;
#endif

error:
#ifdef PYTHON3
    return NULL;
#else
    return;
#endif
}

