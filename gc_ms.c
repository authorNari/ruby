/* Mark Sweep GC */

#define heaps			objspace->heap.ptr
#define heaps_length		objspace->heap.length
#define heaps_used		objspace->heap.used
#define lomem			objspace->heap.range[0]
#define himem			objspace->heap.range[1]
#define heaps_inc		objspace->heap.increment
#define heaps_freed		objspace->heap.freed
#define mark_stack		objspace->markstack.buffer
#define mark_stack_ptr		objspace->markstack.ptr
#define mark_stack_overflow	objspace->markstack.overflow

#include "gc_ms_profiler.c"

static inline void make_deferred(RVALUE *p);
static inline void make_io_deferred(RVALUE *p);
static VALUE lazy_sweep_enable(void);

#include "gc_ms_heap.c"

static void
init_mark_stack(rb_objspace_t *objspace)
{
    mark_stack_overflow = 0;
    mark_stack_ptr = mark_stack;
}

#define MARK_STACK_EMPTY (mark_stack_ptr == mark_stack)

static void gc_mark(rb_objspace_t *objspace, VALUE ptr, int lev);
static void gc_mark_children(rb_objspace_t *objspace, VALUE ptr, int lev);

static void
gc_mark_all(rb_objspace_t *objspace)
{
    RVALUE *p, *pend;
    size_t i;

    init_mark_stack(objspace);
    for (i = 0; i < heaps_used; i++) {
	p = objspace->heap.sorted[i].start; pend = objspace->heap.sorted[i].end;
	while (p < pend) {
	    if (is_marked_object(p, GET_HEAP_BITMAP(p)) &&
		p->as.basic.flags) {
		gc_mark_children(objspace, (VALUE)p, 0);
	    }
	    p++;
	}
    }
}

static void
gc_mark_rest(rb_objspace_t *objspace)
{
    VALUE tmp_arry[MARK_STACK_MAX];
    VALUE *p;

    p = (mark_stack_ptr - mark_stack) + tmp_arry;
    MEMCPY(tmp_arry, mark_stack, VALUE, p - tmp_arry);

    init_mark_stack(objspace);
    while (p != tmp_arry) {
	p--;
	gc_mark_children(objspace, *p, 0);
    }
}

static void
gc_mark(rb_objspace_t *objspace, VALUE ptr, int lev)
{
    register RVALUE *obj;

    obj = RANY(ptr);
    if (rb_special_const_p(ptr)) return; /* special const not marked */
    if (obj->as.basic.flags == 0) return;       /* free cell */
    if (!gc_mark_ptr(objspace, ptr)) return;	/* already marked */

    if (lev > GC_LEVEL_MAX || (lev == 0 && stack_check(STACKFRAME_FOR_GC_MARK))) {
	if (!mark_stack_overflow) {
	    if (mark_stack_ptr - mark_stack < MARK_STACK_MAX) {
		*mark_stack_ptr = ptr;
		mark_stack_ptr++;
	    }
	    else {
		mark_stack_overflow = 1;
	    }
	}
	return;
    }
    gc_mark_children(objspace, ptr, lev+1);
}

static void
gc_mark_children(rb_objspace_t *objspace, VALUE ptr, int lev)
{
    register RVALUE *obj = RANY(ptr);

    goto marking;		/* skip */

  again:
    obj = RANY(ptr);
    if (rb_special_const_p(ptr)) return; /* special const not marked */
    if (obj->as.basic.flags == 0) return;       /* free cell */
    if (!gc_mark_ptr(objspace, ptr)) return;	/* already marked */

  marking:
    if (FL_TEST(obj, FL_EXIVAR)) {
	rb_mark_generic_ivar(ptr);
    }

    switch (BUILTIN_TYPE(obj)) {
      case T_NIL:
      case T_FIXNUM:
	rb_bug("rb_gc_mark() called for broken object");
	break;

      case T_NODE:
	switch (nd_type(obj)) {
	  case NODE_IF:		/* 1,2,3 */
	  case NODE_FOR:
	  case NODE_ITER:
	  case NODE_WHEN:
	  case NODE_MASGN:
	  case NODE_RESCUE:
	  case NODE_RESBODY:
	  case NODE_CLASS:
	  case NODE_BLOCK_PASS:
	    gc_mark(objspace, (VALUE)obj->as.node.u2.node, lev);
	    /* fall through */
	  case NODE_BLOCK:	/* 1,3 */
	  case NODE_OPTBLOCK:
	  case NODE_ARRAY:
	  case NODE_DSTR:
	  case NODE_DXSTR:
	  case NODE_DREGX:
	  case NODE_DREGX_ONCE:
	  case NODE_ENSURE:
	  case NODE_CALL:
	  case NODE_DEFS:
	  case NODE_OP_ASGN1:
	    gc_mark(objspace, (VALUE)obj->as.node.u1.node, lev);
	    /* fall through */
	  case NODE_SUPER:	/* 3 */
	  case NODE_FCALL:
	  case NODE_DEFN:
	  case NODE_ARGS_AUX:
	    ptr = (VALUE)obj->as.node.u3.node;
	    goto again;

	  case NODE_WHILE:	/* 1,2 */
	  case NODE_UNTIL:
	  case NODE_AND:
	  case NODE_OR:
	  case NODE_CASE:
	  case NODE_SCLASS:
	  case NODE_DOT2:
	  case NODE_DOT3:
	  case NODE_FLIP2:
	  case NODE_FLIP3:
	  case NODE_MATCH2:
	  case NODE_MATCH3:
	  case NODE_OP_ASGN_OR:
	  case NODE_OP_ASGN_AND:
	  case NODE_MODULE:
	  case NODE_ALIAS:
	  case NODE_VALIAS:
	  case NODE_ARGSCAT:
	    gc_mark(objspace, (VALUE)obj->as.node.u1.node, lev);
	    /* fall through */
	  case NODE_GASGN:	/* 2 */
	  case NODE_LASGN:
	  case NODE_DASGN:
	  case NODE_DASGN_CURR:
	  case NODE_IASGN:
	  case NODE_IASGN2:
	  case NODE_CVASGN:
	  case NODE_COLON3:
	  case NODE_OPT_N:
	  case NODE_EVSTR:
	  case NODE_UNDEF:
	  case NODE_POSTEXE:
	    ptr = (VALUE)obj->as.node.u2.node;
	    goto again;

	  case NODE_HASH:	/* 1 */
	  case NODE_LIT:
	  case NODE_STR:
	  case NODE_XSTR:
	  case NODE_DEFINED:
	  case NODE_MATCH:
	  case NODE_RETURN:
	  case NODE_BREAK:
	  case NODE_NEXT:
	  case NODE_YIELD:
	  case NODE_COLON2:
	  case NODE_SPLAT:
	  case NODE_TO_ARY:
	    ptr = (VALUE)obj->as.node.u1.node;
	    goto again;

	  case NODE_SCOPE:	/* 2,3 */
	  case NODE_CDECL:
	  case NODE_OPT_ARG:
	    gc_mark(objspace, (VALUE)obj->as.node.u3.node, lev);
	    ptr = (VALUE)obj->as.node.u2.node;
	    goto again;

	  case NODE_ARGS:	/* custom */
	    {
		struct rb_args_info *args = obj->as.node.u3.args;
		if (args) {
		    if (args->pre_init)    gc_mark(objspace, (VALUE)args->pre_init, lev);
		    if (args->post_init)   gc_mark(objspace, (VALUE)args->post_init, lev);
		    if (args->opt_args)    gc_mark(objspace, (VALUE)args->opt_args, lev);
		    if (args->kw_args)     gc_mark(objspace, (VALUE)args->kw_args, lev);
		    if (args->kw_rest_arg) gc_mark(objspace, (VALUE)args->kw_rest_arg, lev);
		}
	    }
	    ptr = (VALUE)obj->as.node.u2.node;
	    goto again;

	  case NODE_ZARRAY:	/* - */
	  case NODE_ZSUPER:
	  case NODE_VCALL:
	  case NODE_GVAR:
	  case NODE_LVAR:
	  case NODE_DVAR:
	  case NODE_IVAR:
	  case NODE_CVAR:
	  case NODE_NTH_REF:
	  case NODE_BACK_REF:
	  case NODE_REDO:
	  case NODE_RETRY:
	  case NODE_SELF:
	  case NODE_NIL:
	  case NODE_TRUE:
	  case NODE_FALSE:
	  case NODE_ERRINFO:
	  case NODE_BLOCK_ARG:
	    break;
	  case NODE_ALLOCA:
	    mark_locations_array(objspace,
				 (VALUE*)obj->as.node.u1.value,
				 obj->as.node.u3.cnt);
	    ptr = (VALUE)obj->as.node.u2.node;
	    goto again;

	  default:		/* unlisted NODE */
	    if (is_pointer_to_heap(objspace, obj->as.node.u1.node)) {
		gc_mark(objspace, (VALUE)obj->as.node.u1.node, lev);
	    }
	    if (is_pointer_to_heap(objspace, obj->as.node.u2.node)) {
		gc_mark(objspace, (VALUE)obj->as.node.u2.node, lev);
	    }
	    if (is_pointer_to_heap(objspace, obj->as.node.u3.node)) {
		gc_mark(objspace, (VALUE)obj->as.node.u3.node, lev);
	    }
	}
	return;			/* no need to mark class. */
    }

    gc_mark(objspace, obj->as.basic.klass, lev);
    switch (BUILTIN_TYPE(obj)) {
      case T_ICLASS:
      case T_CLASS:
      case T_MODULE:
	mark_m_tbl(objspace, RCLASS_M_TBL(obj), lev);
	if (!RCLASS_EXT(obj)) break;
	mark_tbl(objspace, RCLASS_IV_TBL(obj), lev);
	mark_const_tbl(objspace, RCLASS_CONST_TBL(obj), lev);
	ptr = RCLASS_SUPER(obj);
	goto again;

      case T_ARRAY:
	if (FL_TEST(obj, ELTS_SHARED)) {
	    ptr = obj->as.array.as.heap.aux.shared;
	    goto again;
	}
	else {
	    long i, len = RARRAY_LEN(obj);
	    VALUE *ptr = RARRAY_PTR(obj);
	    for (i=0; i < len; i++) {
		gc_mark(objspace, *ptr++, lev);
	    }
	}
	break;

      case T_HASH:
	mark_hash(objspace, obj->as.hash.ntbl, lev);
	ptr = obj->as.hash.ifnone;
	goto again;

      case T_STRING:
#define STR_ASSOC FL_USER3   /* copied from string.c */
	if (FL_TEST(obj, RSTRING_NOEMBED) && FL_ANY(obj, ELTS_SHARED|STR_ASSOC)) {
	    ptr = obj->as.string.as.heap.aux.shared;
	    goto again;
	}
	break;

      case T_DATA:
	if (RTYPEDDATA_P(obj)) {
	    RUBY_DATA_FUNC mark_func = obj->as.typeddata.type->function.dmark;
	    if (mark_func) (*mark_func)(DATA_PTR(obj));
	}
	else {
	    if (obj->as.data.dmark) (*obj->as.data.dmark)(DATA_PTR(obj));
	}
	break;

      case T_OBJECT:
        {
            long i, len = ROBJECT_NUMIV(obj);
	    VALUE *ptr = ROBJECT_IVPTR(obj);
            for (i  = 0; i < len; i++) {
		gc_mark(objspace, *ptr++, lev);
            }
        }
	break;

      case T_FILE:
        if (obj->as.file.fptr) {
            gc_mark(objspace, obj->as.file.fptr->pathv, lev);
            gc_mark(objspace, obj->as.file.fptr->tied_io_for_writing, lev);
            gc_mark(objspace, obj->as.file.fptr->writeconv_asciicompat, lev);
            gc_mark(objspace, obj->as.file.fptr->writeconv_pre_ecopts, lev);
            gc_mark(objspace, obj->as.file.fptr->encs.ecopts, lev);
            gc_mark(objspace, obj->as.file.fptr->write_lock, lev);
        }
        break;

      case T_REGEXP:
        gc_mark(objspace, obj->as.regexp.src, lev);
        break;

      case T_FLOAT:
      case T_BIGNUM:
      case T_ZOMBIE:
	break;

      case T_MATCH:
	gc_mark(objspace, obj->as.match.regexp, lev);
	if (obj->as.match.str) {
	    ptr = obj->as.match.str;
	    goto again;
	}
	break;

      case T_RATIONAL:
	gc_mark(objspace, obj->as.rational.num, lev);
	gc_mark(objspace, obj->as.rational.den, lev);
	break;

      case T_COMPLEX:
	gc_mark(objspace, obj->as.complex.real, lev);
	gc_mark(objspace, obj->as.complex.imag, lev);
	break;

      case T_STRUCT:
	{
	    long len = RSTRUCT_LEN(obj);
	    VALUE *ptr = RSTRUCT_PTR(obj);

	    while (len--) {
		gc_mark(objspace, *ptr++, lev);
	    }
	}
	break;

      default:
	rb_bug("rb_gc_mark(): unknown data type 0x%x(%p) %s",
	       BUILTIN_TYPE(obj), (void *)obj,
	       is_pointer_to_heap(objspace, obj) ? "corrupted object" : "non object");
    }
}

static void
slot_sweep(rb_objspace_t *objspace, struct heaps_slot *sweep_slot)
{
    size_t free_num = 0, final_num = 0;
    RVALUE *p, *pend;
    RVALUE *final = deferred_final_list;
    int deferred;
    uintptr_t *bits;

    p = sweep_slot->slot; pend = p + sweep_slot->limit;
    bits = GET_HEAP_BITMAP(p);
    while (p < pend) {
        if ((!(is_marked_object(p, bits))) && BUILTIN_TYPE(p) != T_ZOMBIE) {
            if (p->as.basic.flags) {
                if ((deferred = obj_free(objspace, (VALUE)p)) ||
                    (FL_TEST(p, FL_FINALIZE))) {
                    if (!deferred) {
                        p->as.free.flags = T_ZOMBIE;
                        RDATA(p)->dfree = 0;
                    }
                    p->as.free.next = deferred_final_list;
                    deferred_final_list = p;
                    assert(BUILTIN_TYPE(p) == T_ZOMBIE);
                    final_num++;
                }
                else {
                    VALGRIND_MAKE_MEM_UNDEFINED((void*)p, sizeof(RVALUE));
                    p->as.free.flags = 0;
                    p->as.free.next = sweep_slot->freelist;
                    sweep_slot->freelist = p;
                    free_num++;
                }
            }
            else {
                free_num++;
            }
        }
        p++;
    }
    gc_clear_slot_bits(sweep_slot);
    if (final_num + free_num == sweep_slot->limit &&
        objspace->heap.free_num > objspace->heap.do_heap_free) {
        RVALUE *pp;

        for (pp = deferred_final_list; pp != final; pp = pp->as.free.next) {
	    RDATA(pp)->dmark = (void (*)(void *))(VALUE)sweep_slot;
            pp->as.free.flags |= FL_SINGLETON; /* freeing page mark */
        }
        sweep_slot->limit = final_num;
        unlink_heap_slot(objspace, sweep_slot);
    }
    else {
        if (free_num > 0) {
            link_free_heap_slot(objspace, sweep_slot);
        }
        else {
            sweep_slot->free_next = NULL;
        }
        objspace->heap.free_num += free_num;
    }
    objspace->heap.final_num += final_num;

    if (deferred_final_list && !finalizing) {
        rb_thread_t *th = GET_THREAD();
        if (th) {
            RUBY_VM_SET_FINALIZER_INTERRUPT(th);
        }
    }
}

static int
ready_to_gc(rb_objspace_t *objspace)
{
    if (dont_gc || during_gc) {
	if (!has_free_object) {
            if (!heaps_increment(objspace)) {
                set_heaps_increment(objspace);
                heaps_increment(objspace);
            }
	}
	return FALSE;
    }
    return TRUE;
}

static void
before_gc_sweep(rb_objspace_t *objspace)
{
    objspace->heap.do_heap_free = (size_t)((heaps_used * HEAP_OBJ_LIMIT) * 0.65);
    objspace->heap.free_min = (size_t)((heaps_used * HEAP_OBJ_LIMIT)  * 0.2);
    if (objspace->heap.free_min < initial_free_min) {
	objspace->heap.do_heap_free = heaps_used * HEAP_OBJ_LIMIT;
        objspace->heap.free_min = initial_free_min;
    }
    objspace->heap.sweep_slots = heaps;
    objspace->heap.free_num = 0;
    objspace->heap.free_slots = NULL;

    /* sweep unlinked method entries */
    if (GET_VM()->unlinked_method_entry_list) {
	rb_sweep_method_entry(GET_VM());
    }
}

static void
after_gc_sweep(rb_objspace_t *objspace)
{
    size_t inc;
    GC_PROF_SET_MALLOC_INFO;

    if (objspace->heap.free_num < objspace->heap.free_min) {
        set_heaps_increment(objspace);
        heaps_increment(objspace);
    }

    inc = ATOMIC_SIZE_EXCHANGE(malloc_increase, 0);
    if (inc > malloc_limit) {
	malloc_limit += (size_t)((inc - malloc_limit) * (double)objspace->heap.live_num / (heaps_used * HEAP_OBJ_LIMIT));
	if (malloc_limit < initial_malloc_limit) malloc_limit = initial_malloc_limit;
    }

    free_unused_heaps(objspace);
}

static int
lazy_sweep(rb_objspace_t *objspace)
{
    struct heaps_slot *next;

    heaps_increment(objspace);
    while (objspace->heap.sweep_slots) {
        next = objspace->heap.sweep_slots->next;
	slot_sweep(objspace, objspace->heap.sweep_slots);
        objspace->heap.sweep_slots = next;
        if (has_free_object) {
            during_gc = 0;
            return TRUE;
        }
    }
    return FALSE;
}

static void
rest_sweep(rb_objspace_t *objspace)
{
    if (objspace->heap.sweep_slots) {
	while (objspace->heap.sweep_slots) {
	    lazy_sweep(objspace);
	}
	after_gc_sweep(objspace);
    }
}

static void gc_marks(rb_objspace_t *objspace);

static int
gc_lazy_sweep(rb_objspace_t *objspace)
{
    int res;
    INIT_GC_PROF_PARAMS;

    if (objspace->flags.dont_lazy_sweep)
        return garbage_collect(objspace);


    if (!ready_to_gc(objspace)) return TRUE;

    during_gc++;
    GC_PROF_TIMER_START;
    GC_PROF_SWEEP_TIMER_START;

    if (objspace->heap.sweep_slots) {
        res = lazy_sweep(objspace);
        if (res) {
            GC_PROF_SWEEP_TIMER_STOP;
            GC_PROF_SET_MALLOC_INFO;
            GC_PROF_TIMER_STOP(Qfalse);
            return res;
        }
        after_gc_sweep(objspace);
    }
    else {
        if (heaps_increment(objspace)) {
            during_gc = 0;
            return TRUE;
        }
    }

    gc_marks(objspace);

    before_gc_sweep(objspace);
    if (objspace->heap.free_min > (heaps_used * HEAP_OBJ_LIMIT - objspace->heap.live_num)) {
	set_heaps_increment(objspace);
    }

    GC_PROF_SWEEP_TIMER_START;
    if (!(res = lazy_sweep(objspace))) {
        after_gc_sweep(objspace);
        if (has_free_object) {
            res = TRUE;
            during_gc = 0;
        }
    }
    GC_PROF_SWEEP_TIMER_STOP;

    GC_PROF_TIMER_STOP(Qtrue);
    return res;
}

static void
gc_sweep(rb_objspace_t *objspace)
{
    struct heaps_slot *next;

    before_gc_sweep(objspace);

    while (objspace->heap.sweep_slots) {
        next = objspace->heap.sweep_slots->next;
	slot_sweep(objspace, objspace->heap.sweep_slots);
        objspace->heap.sweep_slots = next;
    }

    after_gc_sweep(objspace);

    during_gc = 0;
}

static void
force_recycle(rb_objspace_t *objspace, VALUE p)
{
    struct heaps_slot *slot;

    if (is_marked_object((RVALUE *)p, GET_HEAP_BITMAP(p))) {
        add_slot_local_freelist(objspace, (RVALUE *)p);
    }
    else {
        GC_PROF_DEC_LIVE_NUM;
        slot = add_slot_local_freelist(objspace, (RVALUE *)p);
        if (slot->free_next == NULL) {
            link_free_heap_slot(objspace, slot);
        }
    }
}

static inline void
recycle_finalized_obj(rb_objspace_t *objspace, VALUE p)
{
    if (!FL_TEST(p, FL_SINGLETON)) { /* not freeing page */
        add_slot_local_freelist(objspace, (RVALUE *)p);
        if (!is_lazy_sweeping(objspace)) {
            GC_PROF_DEC_LIVE_NUM;
        }
    }
    else {
        struct heaps_slot *slot = (struct heaps_slot *)(VALUE)RDATA(p)->dmark;
        slot->limit--;
    }
}

static inline void
make_deferred(RVALUE *p)
{
    p->as.basic.flags = (p->as.basic.flags & ~T_MASK) | T_ZOMBIE;
}

static inline void
make_io_deferred(RVALUE *p)
{
    rb_io_t *fptr = p->as.file.fptr;
    make_deferred(p);
    p->as.data.dfree = (void (*)(void*))rb_io_fptr_finalize;
    p->as.data.data = fptr;
}

static void
gc_marks(rb_objspace_t *objspace)
{
    struct gc_list *list;
    rb_thread_t *th = GET_THREAD();
    GC_PROF_MARK_TIMER_START;

    objspace->heap.live_num = 0;
    objspace->base.count++;


    SET_STACK_END;

    init_mark_stack(objspace);

    th->vm->self ? rb_gc_mark(th->vm->self) : rb_vm_mark(th->vm);

    mark_tbl(objspace, finalizer_table, 0);
    mark_current_machine_context(objspace, th);

    rb_gc_mark_symbols();
    rb_gc_mark_encodings();

    /* mark protected global variables */
    for (list = global_List; list; list = list->next) {
	rb_gc_mark_maybe(*list->varptr);
    }
    rb_mark_end_proc();
    rb_gc_mark_global_tbl();

    mark_tbl(objspace, rb_class_tbl, 0);

    /* mark generic instance variables for special constants */
    rb_mark_generic_ivar_tbl();

    rb_gc_mark_parser();

    rb_gc_mark_unlinked_live_method_entries(th->vm);

    /* gc_mark objects whose marking are not completed*/
    while (!MARK_STACK_EMPTY) {
	if (mark_stack_overflow) {
	    gc_mark_all(objspace);
	}
	else {
	    gc_mark_rest(objspace);
	}
    }
    GC_PROF_MARK_TIMER_STOP;
}

#define GC_NOTIFY 0

static int
garbage_collect(rb_objspace_t *objspace)
{
    INIT_GC_PROF_PARAMS;

    if (GC_NOTIFY) printf("start garbage_collect()\n");

    if (!heaps) {
	return FALSE;
    }
    if (!ready_to_gc(objspace)) {
        return TRUE;
    }

    GC_PROF_TIMER_START;

    rest_sweep(objspace);

    during_gc++;
    gc_marks(objspace);

    GC_PROF_SWEEP_TIMER_START;
    gc_sweep(objspace);
    GC_PROF_SWEEP_TIMER_STOP;

    GC_PROF_TIMER_STOP(Qtrue);
    if (GC_NOTIFY) printf("end garbage_collect()\n");
    return TRUE;
}

static VALUE
lazy_sweep_enable(void)
{
    rb_objspace_t *objspace = &rb_objspace;

    objspace->flags.dont_lazy_sweep = FALSE;
    return Qnil;
}

static void
rb_objspace_call_finalizer(rb_objspace_t *objspace)
{
    RVALUE *p, *pend;
    RVALUE *final_list = 0;
    size_t i;

    /* run finalizers */
    rest_sweep(objspace);

    if (ATOMIC_EXCHANGE(finalizing, 1)) return;

    do {
	/* XXX: this loop will make no sense */
	/* because mark will not be removed */
	finalize_deferred(objspace);
	mark_tbl(objspace, finalizer_table, 0);
	st_foreach(finalizer_table, chain_finalized_object,
		   (st_data_t)&deferred_final_list);
    } while (deferred_final_list);
    /* force to run finalizer */
    while (finalizer_table->num_entries) {
	struct force_finalize_list *list = 0;
	st_foreach(finalizer_table, force_chain_object, (st_data_t)&list);
	while (list) {
	    struct force_finalize_list *curr = list;
	    st_data_t obj = (st_data_t)curr->obj;
	    run_finalizer(objspace, curr->obj, curr->table);
	    st_delete(finalizer_table, &obj, 0);
	    list = curr->next;
	    xfree(curr);
	}
    }

    /* finalizers are part of garbage collection */
    during_gc++;

    /* run data object's finalizers */
    for (i = 0; i < heaps_used; i++) {
	p = objspace->heap.sorted[i].start; pend = objspace->heap.sorted[i].end;
	while (p < pend) {
	    if (BUILTIN_TYPE(p) == T_DATA &&
		DATA_PTR(p) && RANY(p)->as.data.dfree &&
		!rb_obj_is_thread((VALUE)p) && !rb_obj_is_mutex((VALUE)p) &&
		!rb_obj_is_fiber((VALUE)p)) {
		p->as.free.flags = 0;
		if (RTYPEDDATA_P(p)) {
		    RDATA(p)->dfree = RANY(p)->as.typeddata.type->function.dfree;
		}
		if (RANY(p)->as.data.dfree == (RUBY_DATA_FUNC)-1) {
		    xfree(DATA_PTR(p));
		}
		else if (RANY(p)->as.data.dfree) {
		    make_deferred(RANY(p));
		    RANY(p)->as.free.next = final_list;
		    final_list = p;
		}
	    }
	    else if (BUILTIN_TYPE(p) == T_FILE) {
		if (RANY(p)->as.file.fptr) {
		    make_io_deferred(RANY(p));
		    RANY(p)->as.free.next = final_list;
		    final_list = p;
		}
	    }
	    p++;
	}
    }
    during_gc = 0;
    if (final_list) {
	finalize_list(objspace, final_list);
    }

    st_free_table(finalizer_table);
    finalizer_table = 0;
    ATOMIC_SET(finalizing, 0);
}
