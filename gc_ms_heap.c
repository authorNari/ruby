#ifndef GC_BITMAP_MARKING
#define GC_BITMAP_MARKING 1
#endif

#ifndef HEAP_ALIGN_LOG
/* default tiny heap size: 16KB */
#define HEAP_ALIGN_LOG 14
#endif
#define HEAP_ALIGN (1UL << HEAP_ALIGN_LOG)
#define HEAP_ALIGN_MASK (~(~0UL << HEAP_ALIGN_LOG))
#define REQUIRED_SIZE_BY_MALLOC (sizeof(size_t) * 5)
#define HEAP_SIZE (HEAP_ALIGN - REQUIRED_SIZE_BY_MALLOC)
#define CEILDIV(i, mod) (((i) + (mod) - 1)/(mod))

#define HEAP_OBJ_LIMIT (unsigned int)((HEAP_SIZE - sizeof(struct heaps_header))/sizeof(struct RVALUE))
#define HEAP_BITMAP_LIMIT CEILDIV(CEILDIV(HEAP_SIZE, sizeof(struct RVALUE)), sizeof(uintptr_t)*8)

#define GET_HEAP_HEADER(x) (HEAP_HEADER((uintptr_t)(x) & ~(HEAP_ALIGN_MASK)))
#define GET_HEAP_SLOT(x) (GET_HEAP_HEADER(x)->base)
#if GC_BITMAP_MARKING
#define GET_HEAP_BITMAP(x) (GET_HEAP_HEADER(x)->bits)
#else
#define GET_HEAP_BITMAP(x) NULL
#endif
#define NUM_IN_SLOT(p) (((uintptr_t)(p) & HEAP_ALIGN_MASK)/sizeof(RVALUE))
#define BITMAP_INDEX(p) (NUM_IN_SLOT(p) / (sizeof(uintptr_t) * 8))
#define BITMAP_OFFSET(p) (NUM_IN_SLOT(p) & ((sizeof(uintptr_t) * 8)-1))
#define MARKED_IN_BITMAP(bits, p) (bits[BITMAP_INDEX(p)] & ((uintptr_t)1 << BITMAP_OFFSET(p)))
#define MARK_IN_BITMAP(bits, p) (bits[BITMAP_INDEX(p)] = bits[BITMAP_INDEX(p)] | ((uintptr_t)1 << BITMAP_OFFSET(p)))
#define CLEAR_IN_BITMAP(bits, p) (bits[BITMAP_INDEX(p)] &= ~((uintptr_t)1 << BITMAP_OFFSET(p)))


#if defined(ENABLE_VM_OBJSPACE) && ENABLE_VM_OBJSPACE
rb_objspace_t *
rb_objspace_alloc(void)
{
    rb_objspace_t *objspace = malloc(sizeof(rb_objspace_t));
    memset(objspace, 0, sizeof(*objspace));
    malloc_limit = initial_malloc_limit;
    ruby_gc_stress = ruby_initial_gc_stress;

    return objspace;
}

static void gc_sweep(rb_objspace_t *);
static void slot_sweep(rb_objspace_t *, struct heaps_slot *);
static void rest_sweep(rb_objspace_t *);

void
rb_objspace_free(rb_objspace_t *objspace)
{
    rest_sweep(objspace);
    if (objspace->profile.record) {
	free(objspace->profile.record);
	objspace->profile.record = 0;
    }
    if (global_List) {
	struct gc_list *list, *next;
	for (list = global_List; list; list = next) {
	    next = list->next;
	    xfree(list);
	}
    }
    if (objspace->heap.free_bitmap) {
        struct heaps_free_bitmap *list, *next;
        for (list = objspace->heap.free_bitmap; list; list = next) {
            next = list->next;
            free(list);
        }
    }
    if (objspace->heap.sorted) {
	size_t i;
	for (i = 0; i < heaps_used; ++i) {
            free(objspace->heap.sorted[i].slot->bits);
	    aligned_free(objspace->heap.sorted[i].slot->membase);
            free(objspace->heap.sorted[i].slot);
	}
	free(objspace->heap.sorted);
	heaps_used = 0;
	heaps = 0;
    }
    free(objspace);
}
#endif

#define is_lazy_sweeping(objspace) ((objspace)->heap.sweep_slots != 0)

#define HEAP_HEADER(p) ((struct heaps_header *)(p))

static int gc_lazy_sweep(rb_objspace_t *objspace);

static void
allocate_sorted_heaps(rb_objspace_t *objspace, size_t next_heaps_length)
{
    struct sorted_heaps_slot *p;
    struct heaps_free_bitmap *bits;
    size_t size, add, i;

    size = next_heaps_length*sizeof(struct sorted_heaps_slot);
    add = next_heaps_length - heaps_used;

    if (heaps_used > 0) {
	p = (struct sorted_heaps_slot *)realloc(objspace->heap.sorted, size);
	if (p) objspace->heap.sorted = p;
    }
    else {
	p = objspace->heap.sorted = (struct sorted_heaps_slot *)malloc(size);
    }

    if (p == 0) {
	during_gc = 0;
	rb_memerror();
    }

    for (i = 0; i < add; i++) {
        bits = (struct heaps_free_bitmap *)malloc(HEAP_BITMAP_LIMIT * sizeof(uintptr_t));
        if (bits == 0) {
            during_gc = 0;
            rb_memerror();
            return;
        }
        bits->next = objspace->heap.free_bitmap;
        objspace->heap.free_bitmap = bits;
    }
}

static void
link_free_heap_slot(rb_objspace_t *objspace, struct heaps_slot *slot)
{
    slot->free_next = objspace->heap.free_slots;
    objspace->heap.free_slots = slot;
}

static void
unlink_free_heap_slot(rb_objspace_t *objspace, struct heaps_slot *slot)
{
    objspace->heap.free_slots = slot->free_next;
    slot->free_next = NULL;
}

static void
assign_heap_slot(rb_objspace_t *objspace)
{
    RVALUE *p, *pend, *membase;
    struct heaps_slot *slot;
    size_t hi, lo, mid;
    size_t objs;

    objs = HEAP_OBJ_LIMIT;
    p = (RVALUE*)aligned_malloc(HEAP_ALIGN, HEAP_SIZE);
    if (p == 0) {
	during_gc = 0;
	rb_memerror();
    }
    slot = (struct heaps_slot *)malloc(sizeof(struct heaps_slot));
    if (slot == 0) {
       aligned_free(p);
       during_gc = 0;
       rb_memerror();
    }
    MEMZERO((void*)slot, struct heaps_slot, 1);

    slot->next = heaps;
    if (heaps) heaps->prev = slot;
    heaps = slot;

    membase = p;
    p = (RVALUE*)((VALUE)p + sizeof(struct heaps_header));
    if ((VALUE)p % sizeof(RVALUE) != 0) {
       p = (RVALUE*)((VALUE)p + sizeof(RVALUE) - ((VALUE)p % sizeof(RVALUE)));
       objs = (HEAP_SIZE - (size_t)((VALUE)p - (VALUE)membase))/sizeof(RVALUE);
    }

    lo = 0;
    hi = heaps_used;
    while (lo < hi) {
	register RVALUE *mid_membase;
	mid = (lo + hi) / 2;
        mid_membase = objspace->heap.sorted[mid].slot->membase;
	if (mid_membase < membase) {
	    lo = mid + 1;
	}
	else if (mid_membase > membase) {
	    hi = mid;
	}
	else {
	    rb_bug("same heap slot is allocated: %p at %"PRIuVALUE, (void *)membase, (VALUE)mid);
	}
    }
    if (hi < heaps_used) {
	MEMMOVE(&objspace->heap.sorted[hi+1], &objspace->heap.sorted[hi], struct sorted_heaps_slot, heaps_used - hi);
    }
    objspace->heap.sorted[hi].slot = slot;
    objspace->heap.sorted[hi].start = p;
    objspace->heap.sorted[hi].end = (p + objs);
    heaps->membase = membase;
    heaps->slot = p;
    heaps->limit = objs;
    assert(objspace->heap.free_bitmap != NULL);
    heaps->bits = (uintptr_t *)objspace->heap.free_bitmap;
    objspace->heap.free_bitmap = objspace->heap.free_bitmap->next;
    HEAP_HEADER(membase)->base = heaps;
    HEAP_HEADER(membase)->bits = heaps->bits;
    memset(heaps->bits, 0, HEAP_BITMAP_LIMIT * sizeof(uintptr_t));
    objspace->heap.free_num += objs;
    pend = p + objs;
    if (lomem == 0 || lomem > p) lomem = p;
    if (himem < pend) himem = pend;
    heaps_used++;

    while (p < pend) {
	p->as.free.flags = 0;
	p->as.free.next = heaps->freelist;
	heaps->freelist = p;
	p++;
    }
    link_free_heap_slot(objspace, heaps);
}

static void
add_heap_slots(rb_objspace_t *objspace, size_t add)
{
    size_t i;
    size_t next_heaps_length;

    next_heaps_length = heaps_used + add;

    if (next_heaps_length > heaps_length) {
        allocate_sorted_heaps(objspace, next_heaps_length);
        heaps_length = next_heaps_length;
    }

    for (i = 0; i < add; i++) {
        assign_heap_slot(objspace);
    }
    heaps_inc = 0;
}

static void
init_heap(rb_objspace_t *objspace)
{
    add_heap_slots(objspace, HEAP_MIN_SLOTS / HEAP_OBJ_LIMIT);
    objspace->profile.invoke_time = getrusage_time();
}

static void
initial_expand_heap(rb_objspace_t *objspace)
{
    size_t min_size = initial_heap_min_slots / HEAP_OBJ_LIMIT;

    if (min_size > heaps_used) {
        add_heap_slots(objspace, min_size - heaps_used);
    }
}

static void
set_heaps_increment(rb_objspace_t *objspace)
{
    size_t next_heaps_length = (size_t)(heaps_used * 1.8);

    if (next_heaps_length == heaps_used) {
        next_heaps_length++;
    }

    heaps_inc = next_heaps_length - heaps_used;

    if (next_heaps_length > heaps_length) {
	allocate_sorted_heaps(objspace, next_heaps_length);
        heaps_length = next_heaps_length;
    }
}

static int
heaps_increment(rb_objspace_t *objspace)
{
    if (heaps_inc > 0) {
        assign_heap_slot(objspace);
	heaps_inc--;
	return TRUE;
    }
    return FALSE;
}

static VALUE
obj_alloc(rb_objspace_t *objspace)
{
    VALUE obj;

    if (UNLIKELY(during_gc)) {
	dont_gc = 1;
	during_gc = 0;
	rb_bug("object allocation during garbage collection phase");
    }

    if (UNLIKELY(ruby_gc_stress && !ruby_disable_gc_stress)) {
	if (!garbage_collect(objspace)) {
	    during_gc = 0;
	    rb_memerror();
	}
    }

    if (UNLIKELY(!has_free_object)) {
	if (!gc_lazy_sweep(objspace)) {
	    during_gc = 0;
	    rb_memerror();
	}
    }

    obj = (VALUE)objspace->heap.free_slots->freelist;
    objspace->heap.free_slots->freelist = RANY(obj)->as.free.next;
    if (objspace->heap.free_slots->freelist == NULL) {
        unlink_free_heap_slot(objspace, objspace->heap.free_slots);
    }

    MEMZERO((void*)obj, RVALUE, 1);
#ifdef GC_DEBUG
    RANY(obj)->file = rb_sourcefile();
    RANY(obj)->line = rb_sourceline();
#endif
    GC_PROF_INC_LIVE_NUM;

    return obj;
}

static int
obj_free(rb_objspace_t *objspace, VALUE obj)
{
    switch (BUILTIN_TYPE(obj)) {
      case T_NIL:
      case T_FIXNUM:
      case T_TRUE:
      case T_FALSE:
	rb_bug("obj_free() called for broken object");
	break;
    }

    if (FL_TEST(obj, FL_EXIVAR)) {
	rb_free_generic_ivar((VALUE)obj);
	FL_UNSET(obj, FL_EXIVAR);
    }

    switch (BUILTIN_TYPE(obj)) {
      case T_OBJECT:
	if (!(RANY(obj)->as.basic.flags & ROBJECT_EMBED) &&
            RANY(obj)->as.object.as.heap.ivptr) {
	    xfree(RANY(obj)->as.object.as.heap.ivptr);
	}
	break;
      case T_MODULE:
      case T_CLASS:
	rb_clear_cache_by_class((VALUE)obj);
        if (RCLASS_M_TBL(obj)) {
            rb_free_m_table(RCLASS_M_TBL(obj));
        }
	if (RCLASS_IV_TBL(obj)) {
	    st_free_table(RCLASS_IV_TBL(obj));
	}
	if (RCLASS_CONST_TBL(obj)) {
	    rb_free_const_table(RCLASS_CONST_TBL(obj));
	}
	if (RCLASS_IV_INDEX_TBL(obj)) {
	    st_free_table(RCLASS_IV_INDEX_TBL(obj));
	}
        xfree(RANY(obj)->as.klass.ptr);
	break;
      case T_STRING:
	rb_str_free(obj);
	break;
      case T_ARRAY:
	rb_ary_free(obj);
	break;
      case T_HASH:
	if (RANY(obj)->as.hash.ntbl) {
	    st_free_table(RANY(obj)->as.hash.ntbl);
	}
	break;
      case T_REGEXP:
	if (RANY(obj)->as.regexp.ptr) {
	    onig_free(RANY(obj)->as.regexp.ptr);
	}
	break;
      case T_DATA:
	if (DATA_PTR(obj)) {
	    if (RTYPEDDATA_P(obj)) {
		RDATA(obj)->dfree = RANY(obj)->as.typeddata.type->function.dfree;
	    }
	    if (RANY(obj)->as.data.dfree == (RUBY_DATA_FUNC)-1) {
		xfree(DATA_PTR(obj));
	    }
	    else if (RANY(obj)->as.data.dfree) {
		make_deferred(RANY(obj));
		return 1;
	    }
	}
	break;
      case T_MATCH:
	if (RANY(obj)->as.match.rmatch) {
            struct rmatch *rm = RANY(obj)->as.match.rmatch;
	    onig_region_free(&rm->regs, 0);
            if (rm->char_offset)
		xfree(rm->char_offset);
	    xfree(rm);
	}
	break;
      case T_FILE:
	if (RANY(obj)->as.file.fptr) {
	    make_io_deferred(RANY(obj));
	    return 1;
	}
	break;
      case T_RATIONAL:
      case T_COMPLEX:
	break;
      case T_ICLASS:
	/* iClass shares table with the module */
	xfree(RANY(obj)->as.klass.ptr);
	break;

      case T_FLOAT:
	break;

      case T_BIGNUM:
	if (!(RBASIC(obj)->flags & RBIGNUM_EMBED_FLAG) && RBIGNUM_DIGITS(obj)) {
	    xfree(RBIGNUM_DIGITS(obj));
	}
	break;
      case T_NODE:
	switch (nd_type(obj)) {
	  case NODE_SCOPE:
	    if (RANY(obj)->as.node.u1.tbl) {
		xfree(RANY(obj)->as.node.u1.tbl);
	    }
	    break;
	  case NODE_ARGS:
	    if (RANY(obj)->as.node.u3.args) {
		xfree(RANY(obj)->as.node.u3.args);
	    }
	    break;
	  case NODE_ALLOCA:
	    xfree(RANY(obj)->as.node.u1.node);
	    break;
	}
	break;			/* no need to free iv_tbl */

      case T_STRUCT:
	if ((RBASIC(obj)->flags & RSTRUCT_EMBED_LEN_MASK) == 0 &&
	    RANY(obj)->as.rstruct.as.heap.ptr) {
	    xfree(RANY(obj)->as.rstruct.as.heap.ptr);
	}
	break;

      default:
	rb_bug("gc_sweep(): unknown data type 0x%x(%p) 0x%"PRIxVALUE,
	       BUILTIN_TYPE(obj), (void*)obj, RBASIC(obj)->flags);
    }

    return 0;
}

static inline struct heaps_slot *
add_slot_local_freelist(rb_objspace_t *objspace, RVALUE *p)
{
    struct heaps_slot *slot;

    VALGRIND_MAKE_MEM_UNDEFINED((void*)p, sizeof(RVALUE));
    p->as.free.flags = 0;
    slot = GET_HEAP_SLOT(p);
    p->as.free.next = slot->freelist;
    slot->freelist = p;

    return slot;
}

static void
unlink_heap_slot(rb_objspace_t *objspace, struct heaps_slot *slot)
{
    if (slot->prev)
        slot->prev->next = slot->next;
    if (slot->next)
        slot->next->prev = slot->prev;
    if (heaps == slot)
        heaps = slot->next;
    if (objspace->heap.sweep_slots == slot)
        objspace->heap.sweep_slots = slot->next;
    slot->prev = NULL;
    slot->next = NULL;
}

static void
free_unused_heaps(rb_objspace_t *objspace)
{
    size_t i, j;
    RVALUE *last = 0;

    for (i = j = 1; j < heaps_used; i++) {
	if (objspace->heap.sorted[i].slot->limit == 0) {
            struct heaps_slot* h = objspace->heap.sorted[i].slot;
            ((struct heaps_free_bitmap *)(h->bits))->next =
                objspace->heap.free_bitmap;
            objspace->heap.free_bitmap = (struct heaps_free_bitmap *)h->bits;
	    if (!last) {
                last = objspace->heap.sorted[i].slot->membase;
	    }
	    else {
		aligned_free(objspace->heap.sorted[i].slot->membase);
	    }
            free(objspace->heap.sorted[i].slot);
	    heaps_used--;
	}
	else {
	    if (i != j) {
		objspace->heap.sorted[j] = objspace->heap.sorted[i];
	    }
	    j++;
	}
    }
    if (last) {
	if (last < heaps_freed) {
	    aligned_free(heaps_freed);
	    heaps_freed = last;
	}
	else {
	    aligned_free(last);
	}
    }
}


static inline int
is_pointer_to_heap(rb_objspace_t *objspace, void *ptr)
{
    register RVALUE *p = RANY(ptr);
    register struct sorted_heaps_slot *heap;
    register size_t hi, lo, mid;

    if (p < lomem || p > himem) return FALSE;
    if ((VALUE)p % sizeof(RVALUE) != 0) return FALSE;

    /* check if p looks like a pointer using bsearch*/
    lo = 0;
    hi = heaps_used;
    while (lo < hi) {
	mid = (lo + hi) / 2;
	heap = &objspace->heap.sorted[mid];
	if (heap->start <= p) {
	    if (p < heap->end)
		return TRUE;
	    lo = mid + 1;
	}
	else {
	    hi = mid;
	}
    }
    return FALSE;
}

static inline int
is_marked_object(RVALUE *p, uintptr_t *bits)
{
#if GC_BITMAP_MARKING
    if (MARKED_IN_BITMAP(bits, p)) return 1;
#else
    if (p->as.free.flags & (FL_RESERVED1)) return 1;
#endif
    return 0;
}

static inline int
is_dead_object(rb_objspace_t *objspace, VALUE ptr)
{
    struct heaps_slot *slot = objspace->heap.sweep_slots;
    if (!is_lazy_sweeping(objspace) || is_marked_object((RVALUE *)ptr, GET_HEAP_BITMAP(ptr)))
	return FALSE;
    while (slot) {
	if ((VALUE)slot->slot <= ptr && ptr < (VALUE)(slot->slot + slot->limit))
	    return TRUE;
	slot = slot->next;
    }
    return FALSE;
}

static inline int
is_live_object(rb_objspace_t *objspace, VALUE ptr)
{
    if (BUILTIN_TYPE(ptr) == 0) return FALSE;
    if (RBASIC(ptr)->klass == 0) return FALSE;
    if (is_dead_object(objspace, ptr)) return FALSE;
    return TRUE;
}

static inline int
is_finalized_object(rb_objspace_t *objspace, RVALUE *p)
{
    if ((p->as.basic.flags & FL_FINALIZE) == FL_FINALIZE &&
        !is_marked_object(p, GET_HEAP_BITMAP(p))) return TRUE;
    return FALSE;
}

static int
gc_mark_ptr(rb_objspace_t *objspace, VALUE ptr)
{
    register uintptr_t *bits = GET_HEAP_BITMAP(ptr);
    if (is_marked_object((RVALUE *)ptr, bits)) return 0;
#if GC_BITMAP_MARKING
    MARK_IN_BITMAP(bits, ptr);
#else
    ((RVALUE *)ptr)->as.free.flags |= FL_RESERVED1;
#endif
    objspace->heap.live_num++;
    return 1;
}

static void
gc_clear_slot_bits(struct heaps_slot *slot)
{
#if GC_BITMAP_MARKING
    memset(GET_HEAP_BITMAP(slot->slot), 0,
           HEAP_BITMAP_LIMIT * sizeof(uintptr_t));
#else
    RVALUE *p, *pend;
    p = slot->slot; pend = p + slot->limit;
    while (p < pend) {
        RBASIC(p)->flags &= ~FL_RESERVED1;
        p++;
    }
#endif
}

typedef int each_obj_callback(void *, void *, size_t, void *);

struct each_obj_args {
    each_obj_callback *callback;
    void *data;
};

static VALUE
each_objects(VALUE arg)
{
    size_t i;
    RVALUE *membase = 0;
    RVALUE *pstart, *pend;
    rb_objspace_t *objspace = &rb_objspace;
    struct each_obj_args *args = (struct each_obj_args *)arg;
    volatile VALUE v;

    i = 0;
    while (i < heaps_used) {
	while (0 < i && (uintptr_t)membase < (uintptr_t)objspace->heap.sorted[i-1].slot->membase)
	    i--;
	while (i < heaps_used && (uintptr_t)objspace->heap.sorted[i].slot->membase <= (uintptr_t)membase)
	    i++;
	if (heaps_used <= i)
	  break;
	membase = objspace->heap.sorted[i].slot->membase;

	pstart = objspace->heap.sorted[i].slot->slot;
	pend = pstart + objspace->heap.sorted[i].slot->limit;

	for (; pstart != pend; pstart++) {
	    if (pstart->as.basic.flags) {
		v = (VALUE)pstart; /* acquire to save this object */
		break;
	    }
	}
	if (pstart != pend) {
	    if ((*args->callback)(pstart, pend, sizeof(RVALUE), args->data)) {
		break;
	    }
	}
    }
    RB_GC_GUARD(v);

    return Qnil;
}

static void
objspace_each_objects(each_obj_callback *callback, void *data)
{
    struct each_obj_args args;
    rb_objspace_t *objspace = &rb_objspace;

    rest_sweep(objspace);
    objspace->flags.dont_lazy_sweep = TRUE;

    args.callback = callback;
    args.data = data;
    rb_ensure(each_objects, (VALUE)&args, lazy_sweep_enable, Qnil);
}

/*
 *  call-seq:
 *     ObjectSpace._id2ref(object_id) -> an_object
 *
 *  Converts an object id to a reference to the object. May not be
 *  called on an object id passed as a parameter to a finalizer.
 *
 *     s = "I am a string"                    #=> "I am a string"
 *     r = ObjectSpace._id2ref(s.object_id)   #=> "I am a string"
 *     r == s                                 #=> true
 *
 */

static VALUE
id2ref(VALUE obj, VALUE objid)
{
    rb_objspace_t *objspace = &rb_objspace;
    VALUE ptr;
    void *p0;

    rb_secure(4);
    ptr = NUM2PTR(objid);
    p0 = (void *)ptr;

    if (ptr == Qtrue) return Qtrue;
    if (ptr == Qfalse) return Qfalse;
    if (ptr == Qnil) return Qnil;
    if (FIXNUM_P(ptr)) return (VALUE)ptr;
    ptr = objid ^ FIXNUM_FLAG;	/* unset FIXNUM_FLAG */

    if ((ptr % sizeof(RVALUE)) == (4 << 2)) {
        ID symid = ptr / sizeof(RVALUE);
        if (rb_id2name(symid) == 0)
	    rb_raise(rb_eRangeError, "%p is not symbol id value", p0);
	return ID2SYM(symid);
    }

    if (!is_id_value(objspace, ptr)) {
	rb_raise(rb_eRangeError, "%p is not id value", p0);
    }
    if (!is_live_object(objspace, ptr)) {
	rb_raise(rb_eRangeError, "%p is recycled object", p0);
    }
    return (VALUE)ptr;
}

/*
 *  call-seq:
 *     ObjectSpace.count_objects([result_hash]) -> hash
 *
 *  Counts objects for each type.
 *
 *  It returns a hash as:
 *  {:TOTAL=>10000, :FREE=>3011, :T_OBJECT=>6, :T_CLASS=>404, ...}
 *
 *  If the optional argument, result_hash, is given,
 *  it is overwritten and returned.
 *  This is intended to avoid probe effect.
 *
 *  The contents of the returned hash is implementation defined.
 *  It may be changed in future.
 *
 *  This method is not expected to work except C Ruby.
 *
 */

static VALUE
count_objects(int argc, VALUE *argv, VALUE os)
{
    rb_objspace_t *objspace = &rb_objspace;
    size_t counts[T_MASK+1];
    size_t freed = 0;
    size_t total = 0;
    size_t i;
    VALUE hash;

    if (rb_scan_args(argc, argv, "01", &hash) == 1) {
        if (!RB_TYPE_P(hash, T_HASH))
            rb_raise(rb_eTypeError, "non-hash given");
    }

    for (i = 0; i <= T_MASK; i++) {
        counts[i] = 0;
    }

    for (i = 0; i < heaps_used; i++) {
        RVALUE *p, *pend;

        p = objspace->heap.sorted[i].start; pend = objspace->heap.sorted[i].end;
        for (;p < pend; p++) {
            if (p->as.basic.flags) {
                counts[BUILTIN_TYPE(p)]++;
            }
            else {
                freed++;
            }
        }
        total += objspace->heap.sorted[i].slot->limit;
    }

    if (hash == Qnil) {
        hash = rb_hash_new();
    }
    else if (!RHASH_EMPTY_P(hash)) {
        st_foreach(RHASH_TBL(hash), set_zero, hash);
    }
    rb_hash_aset(hash, ID2SYM(rb_intern("TOTAL")), SIZET2NUM(total));
    rb_hash_aset(hash, ID2SYM(rb_intern("FREE")), SIZET2NUM(freed));

    for (i = 0; i <= T_MASK; i++) {
        VALUE type;
        switch (i) {
#define COUNT_TYPE(t) case (t): type = ID2SYM(rb_intern(#t)); break;
	    COUNT_TYPE(T_NONE);
	    COUNT_TYPE(T_OBJECT);
	    COUNT_TYPE(T_CLASS);
	    COUNT_TYPE(T_MODULE);
	    COUNT_TYPE(T_FLOAT);
	    COUNT_TYPE(T_STRING);
	    COUNT_TYPE(T_REGEXP);
	    COUNT_TYPE(T_ARRAY);
	    COUNT_TYPE(T_HASH);
	    COUNT_TYPE(T_STRUCT);
	    COUNT_TYPE(T_BIGNUM);
	    COUNT_TYPE(T_FILE);
	    COUNT_TYPE(T_DATA);
	    COUNT_TYPE(T_MATCH);
	    COUNT_TYPE(T_COMPLEX);
	    COUNT_TYPE(T_RATIONAL);
	    COUNT_TYPE(T_NIL);
	    COUNT_TYPE(T_TRUE);
	    COUNT_TYPE(T_FALSE);
	    COUNT_TYPE(T_SYMBOL);
	    COUNT_TYPE(T_FIXNUM);
	    COUNT_TYPE(T_UNDEF);
	    COUNT_TYPE(T_NODE);
	    COUNT_TYPE(T_ICLASS);
	    COUNT_TYPE(T_ZOMBIE);
#undef COUNT_TYPE
          default:              type = INT2NUM(i); break;
        }
        if (counts[i])
            rb_hash_aset(hash, type, SIZET2NUM(counts[i]));
    }

    return hash;
}

/*
 *  call-seq:
 *     GC.stat -> Hash
 *
 *  Returns a Hash containing information about the GC.
 *
 *  The hash includes information about internal statistics about GC such as:
 *
 *    {
 *      :count          => 18,
 *      :heap_used      => 77,
 *      :heap_length    => 77,
 *      :heap_increment => 0,
 *      :heap_live_num  => 23287,
 *      :heap_free_num  => 8115,
 *      :heap_final_num => 0,
 *    }
 *
 *  The contents of the hash are implementation defined and may be changed in
 *  the future.
 *
 *  This method is only expected to work on C Ruby.
 *
 */

static VALUE
gc_stat(int argc, VALUE *argv, VALUE self)
{
    rb_objspace_t *objspace = &rb_objspace;
    VALUE hash;

    if (rb_scan_args(argc, argv, "01", &hash) == 1) {
        if (!RB_TYPE_P(hash, T_HASH))
            rb_raise(rb_eTypeError, "non-hash given");
    }

    if (hash == Qnil) {
        hash = rb_hash_new();
    }

    rest_sweep(objspace);

    rb_hash_aset(hash, ID2SYM(rb_intern("count")), SIZET2NUM(objspace->base.count));

    /* implementation dependent counters */
    rb_hash_aset(hash, ID2SYM(rb_intern("heap_used")), SIZET2NUM(objspace->heap.used));
    rb_hash_aset(hash, ID2SYM(rb_intern("heap_length")), SIZET2NUM(objspace->heap.length));
    rb_hash_aset(hash, ID2SYM(rb_intern("heap_increment")), SIZET2NUM(objspace->heap.increment));
    rb_hash_aset(hash, ID2SYM(rb_intern("heap_live_num")), SIZET2NUM(objspace->heap.live_num));
    rb_hash_aset(hash, ID2SYM(rb_intern("heap_free_num")), SIZET2NUM(objspace->heap.free_num));
    rb_hash_aset(hash, ID2SYM(rb_intern("heap_final_num")), SIZET2NUM(objspace->heap.final_num));
    return hash;
}
