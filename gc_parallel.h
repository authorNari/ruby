#ifndef RUBY_GC_PARALLEL_H
#define RUBY_GC_PARALLEL_H

#define DEQUE_STATS 0

#if DEQUE_STATS
enum deque_stat_type {
    PUSH,
    POP_BOTTOM,
    POP_BOTTOM_WITH_CAS_WIN,
    POP_BOTTOM_WITH_CAS_LOSE,
    POP_TOP,
    OVERFLOW,
    GETBACK,
    LAST_STAT_ID
};
#endif

#ifdef __LP64__
typedef uint32_t half_word;
#else
typedef uint16_t half_word;
#endif
union deque_age {
    VALUE data;
    struct {
        half_word tag;
        half_word top;
    } fields;
};

enum deque_data_type {
    DEQUE_DATA_MARKSTACK_PTR,
    DEQUE_DATA_ARRAY_MARK
};

typedef struct array_mark {
    RVALUE *obj;
    size_t index;
} array_mark_t;


#ifndef __LP64__
/* 4KB / 4 - 2(for next field and malloc header) */
#define PAGE_DATAS_SIZE 1048574
#else
/* 4KB / 8 - 2 */
#define PAGE_DATAS_SIZE 524286
#endif

#define OVERFLOW_STACK_PAGE_CACHE_LIMIT 4

typedef struct stack_page {
    void *datas[PAGE_DATAS_SIZE];
    struct stack_page *next;
} stack_page_t;

typedef struct overflow_stack {
    stack_page_t *page;
    stack_page_t *cache;
    size_t page_index;
    size_t page_size;
    size_t full_page_size;
    size_t cache_size;
    enum deque_data_type type;
} overflow_stack_t;


#define GC_PAR_MARKSTACK_OBJS_SIZE 63

/* 32bit: 128Byte, 64bit: 256Byte */
typedef struct par_markstack {
    VALUE objs[GC_PAR_MARKSTACK_OBJS_SIZE];
    struct par_markstack *next;
} par_markstack_t;

#ifndef __LP64__
#define GC_MSTACK_PTR_DEQUE_SIZE (1 << 14)
#define GC_ARRAY_MARK_DEQUE_SIZE (1 << 12)
#define GC_INIT_PAR_MARKSTACK_SIZE (512 * 1024 * 2 / sizeof(par_markstack_t))
#else
#define GC_MSTACK_PTR_DEQUE_SIZE (1 << 17)
#define GC_ARRAY_MARK_DEQUE_SIZE (1 << 13)
#define GC_INIT_PAR_MARKSTACK_SIZE (512 * 1024 / sizeof(par_markstack_t))
#endif

#define GC_DEQUE_SIZE_MASK() (deque->size - 1)
#define GC_DEQUE_MAX() (deque->size - 2)
#define GC_ARRAY_MARK_DEQUE_STRIDE 512


typedef struct deque {
    void **datas;
    size_t bottom;
    union deque_age age;
    size_t size;
    enum deque_data_type type;
    overflow_stack_t overflow_stack;
    struct {
        par_markstack_t *list;
        size_t index;
        size_t length;
        size_t freed;
        size_t max_freed;
    } markstack;
#if DEQUE_STATS
    size_t deque_stats[LAST_STAT_ID];
#endif
} deque_t;

#if DEQUE_STATS
#define count_deque_stats(type) ++deque->deque_stats[type]
#define count_cancel_deque_stats(type) --deque->deque_stats[type]
#else
#define count_deque_stats(type)
#define count_cancel_deque_stats(type)
#endif

#endif
