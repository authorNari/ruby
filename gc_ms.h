#ifndef RUBY_GC_MS_H
#define RUBY_GC_MS_H

/* GC Profiler */
typedef struct gc_profile_record {
    double gc_time;
    double gc_mark_time;
    double gc_sweep_time;
    double gc_invoke_time;

    size_t heap_use_slots;
    size_t heap_live_objects;
    size_t heap_free_objects;
    size_t heap_total_objects;
    size_t heap_use_size;
    size_t heap_total_size;

    int have_finalize;
    int is_marked;

    size_t allocate_increase;
    size_t allocate_limit;
} gc_profile_record;

typedef struct gc_profile {
    int run;
    gc_profile_record *record;
    size_t count;
    size_t size;
    double invoke_time;
} gc_profile;

/* Collected Heap */
struct heaps_slot {
    void *membase;
    RVALUE *slot;
    size_t limit;
    uintptr_t *bits;
    RVALUE *freelist;
    struct heaps_slot *next;
    struct heaps_slot *prev;
    struct heaps_slot *free_next;
};

struct heaps_header {
    struct heaps_slot *base;
    uintptr_t *bits;
};

struct sorted_heaps_slot {
    RVALUE *start;
    RVALUE *end;
    struct heaps_slot *slot;
};

struct heaps_free_bitmap {
    struct heaps_free_bitmap *next;
};

#define MARK_STACK_MAX 1024

typedef struct rb_objspace {
    rb_base_objspace_t base;
    struct {
	size_t increment;
	struct heaps_slot *ptr;
	struct heaps_slot *sweep_slots;
	struct heaps_slot *free_slots;
	struct sorted_heaps_slot *sorted;
	size_t length;
	size_t used;
        struct heaps_free_bitmap *free_bitmap;
	RVALUE *range[2];
	RVALUE *freed;
	size_t live_num;
	size_t free_num;
	size_t free_min;
	size_t final_num;
	size_t do_heap_free;
    } heap;
    struct {
	VALUE buffer[MARK_STACK_MAX];
	VALUE *ptr;
	int overflow;
    } markstack;
    struct {
	int dont_lazy_sweep;
    } flags;
    gc_profile profile;
} rb_objspace_t;

static inline void recycle_finalized_obj(rb_objspace_t *, VALUE);
static void force_recycle(rb_objspace_t *, VALUE);
static void gc_mark(rb_objspace_t *, VALUE, int);
static int gc_mark_ptr(rb_objspace_t *, VALUE);
static int garbage_collect(rb_objspace_t *);
static void objspace_each_objects(
    int (*callback)(void *, void *, size_t, void *),
    void *);
static void rb_objspace_call_finalizer(rb_objspace_t *);
static void init_heap(rb_objspace_t *);
static void initial_expand_heap(rb_objspace_t *);
static VALUE obj_alloc(rb_objspace_t *);
static int obj_free(rb_objspace_t *, VALUE);
static inline int is_pointer_to_heap(rb_objspace_t *, void *);
static inline int is_dead_object(rb_objspace_t *, VALUE);
static inline int is_live_object(rb_objspace_t *, VALUE);
static inline int is_finalized_object(rb_objspace_t *, RVALUE *);
static VALUE id2ref(VALUE, VALUE);
static VALUE count_objects(int, VALUE *, VALUE);
static VALUE gc_stat(int, VALUE *, VALUE);
static void free_unused_heaps(rb_objspace_t *);

static VALUE gc_profile_record_get(void);
static VALUE gc_profile_result(void);
static VALUE gc_profile_enable_get(VALUE);
static VALUE gc_profile_enable(void);
static VALUE gc_profile_disable(void);
static VALUE gc_profile_clear(void);
static VALUE gc_profile_report(int, VALUE *, VALUE);
static VALUE gc_profile_total_time(VALUE);

#if defined(ENABLE_VM_OBJSPACE) && ENABLE_VM_OBJSPACE
rb_objspace_t * rb_objspace_alloc(void);
void rb_objspace_free(rb_objspace_t *);
#endif

#endif
