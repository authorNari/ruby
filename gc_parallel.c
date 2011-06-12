static void gc_mark(rb_objspace_t *, VALUE, int, rb_gc_par_worker_t *);
static void gc_mark_children(rb_objspace_t *, VALUE, int, rb_gc_par_worker_t *);

static size_t
parallel_worker_threads(void)
{
    size_t cpus;

#ifdef _WIN32
    cpus = 1;
#else
    cpus = sysconf(_SC_NPROCESSORS_ONLN);
#endif

    if (cpus > 8) {
        cpus = 8 + (cpus - 8) * (5/8);
    }
    gc_debug("num_parallel_workers: %d\n", (int)cpus);

    return cpus;
}

static inline size_t
deque_increment(deque_t *deque, size_t index)
{
    return (index + 1) & GC_DEQUE_SIZE_MASK();
}

static inline size_t
deque_decrement(deque_t *deque, size_t index)
{
    return (index - 1) & GC_DEQUE_SIZE_MASK();
}

static inline size_t
raw_size_deque(deque_t *deque, size_t bottom, size_t top)
{
    size_t size;

    size = (bottom - top) & GC_DEQUE_SIZE_MASK();
    gc_assert((size != 0 || bottom == top), "size == 0 at bottom == top\n");
    return size;
}

static inline size_t
size_deque(deque_t *deque, size_t bottom, size_t top)
{
    size_t size;

    size = raw_size_deque(deque, bottom, top);
    if (size == GC_DEQUE_SIZE_MASK())
        return 0;
    return size;
}

static inline int
is_full_deque(deque_t *deque, size_t bottom, size_t top)
{
    gc_assert(size_deque(deque, bottom, top) < deque->size,
              "deque size out of range.\n");
    if (size_deque(deque, bottom, top) == GC_DEQUE_MAX()) {
        return TRUE;
    }
    return FALSE;
}

static inline int
is_empty_deque(deque_t *deque, size_t bottom, size_t top)
{
    if (size_deque(deque, bottom, top) == 0) {
        if (deque->type == DEQUE_DATA_MARKSTACK_PTR && deque->markstack.index > 0) {
            return FALSE;
        }
        return TRUE;
    }
    return FALSE;
}

int
is_deques_empty(rb_gc_par_worker_group_t *wgroup)
{
    size_t i;
    deque_t *deque;

    for (i = 0; i < wgroup->num_workers; i++) {
        deque = wgroup->workers[i].local_deque;
        if (!is_empty_deque(deque, deque->bottom, deque->age.fields.top)) {
            return FALSE;
        }
    }
    return TRUE;
}

static inline VALUE
atomic_compxchg_ptr(VALUE *addr, VALUE old, VALUE new)
{
#if GCC_VERSION_SINCE(4,1,2)
    return __sync_val_compare_and_swap(addr, old, new);
#else
    /* TODO: support for not GCC */
#endif
}

static inline void
order_access_memory_barrier(void)
{
#if GCC_VERSION_SINCE(4,1,2)
    __sync_synchronize();
#else
    /* TODO: support for not GCC */
#endif
}

static void
deque_datas_store(deque_t *deque, size_t index, void *data)
{
    switch (deque->type) {
    case DEQUE_DATA_MARKSTACK_PTR:
        ((VALUE *)deque->datas)[index] = (VALUE)data;
	break;
    case DEQUE_DATA_ARRAY_CONTINUE:
        ((array_continue_t *)deque->datas)[index] = *((array_continue_t *)data);
	break;
    default:
        fprintf(stderr, "[FATAL] deque_datas_store(): unknown type %d\n",
                (int)deque->type);
        rb_memerror();
    }
}

static void *
deque_datas_entry(deque_t *deque, size_t index)
{
    switch (deque->type) {
    case DEQUE_DATA_MARKSTACK_PTR:
        return (void *)((VALUE *)deque->datas)[index];
    case DEQUE_DATA_ARRAY_CONTINUE:
        return (void *)(&((array_continue_t *)deque->datas)[index]);
    default:
        fprintf(stderr, "[FATAL] deque_datas_entry(): unknown type %d\n",
                (int)deque->type);
        rb_memerror();
    }
}

static int
push_bottom(deque_t *deque, void *data)
{
    size_t local_bottom;
    half_word top;

    local_bottom = deque->bottom;
    gc_assert(data != 0, "data is null\n");
    gc_assert(local_bottom < deque->size,
              "local_bottom out of range\n");
    top = deque->age.fields.top;
    gc_assert(size_deque(deque, local_bottom, top) < deque->size, "size out of range\n");
    if (!is_full_deque(deque, local_bottom, top)) {
        gc_assert(size_deque(deque, local_bottom, top) < GC_DEQUE_MAX(),
                  "size out of range\n");
        deque_datas_store(deque, local_bottom, data);
        deque->bottom = deque_increment(deque, local_bottom);
        count_deque_stats(PUSH);
        return TRUE;
    }
    return FALSE;
}

static int
pop_bottom(deque_t *deque, void **data)
{
    union deque_age old_age, new_age, res_age;
    size_t local_bottom;

    old_age = deque->age;
    local_bottom = deque->bottom;
    gc_assert(raw_size_deque(deque, local_bottom, old_age.fields.top) != GC_DEQUE_SIZE_MASK(),
              "bottom == (top - 1)\n");

    if (is_empty_deque(deque, local_bottom, old_age.fields.top))
        return FALSE;

    local_bottom = deque_decrement(deque, local_bottom);
    deque->bottom = local_bottom;

    /* necessary memory barrier. */
    /* order_access_memory_barrier(); */
    *data = deque_datas_entry(deque, local_bottom);
    /* must second read of age after local_bottom decremented.
       The local_bottom decrement is lock on. */
    old_age = deque->age;
    if (size_deque(deque, local_bottom, old_age.fields.top) > 0) {
        gc_assert(raw_size_deque(deque, local_bottom, old_age.fields.top) != GC_DEQUE_SIZE_MASK(),
                  "bottom == (top - 1)\n");
        count_deque_stats(POP_BOTTOM);
        return TRUE;
    }

    /* only one data in deque */
    new_age.fields.top = local_bottom;
    new_age.fields.tag = old_age.fields.tag + 1;
    if (local_bottom == old_age.fields.top) {
        res_age.data = atomic_compxchg_ptr((VALUE *)&deque->age.data,
                                           (VALUE)old_age.data,
                                           (VALUE)new_age.data);
        if (res_age.data == old_age.data) {
            /* before pop_top() */
            gc_assert(raw_size_deque(deque, local_bottom, deque->age.fields.top) != GC_DEQUE_SIZE_MASK(),
                      "bottom == (top - 1)\n");
            count_deque_stats(POP_BOTTOM_WITH_CAS_WIN);
            return TRUE;
        }
    }
    /* after pop_top() */
    count_deque_stats(POP_BOTTOM_WITH_CAS_LOSE);
    deque->age = new_age;
    gc_assert(raw_size_deque(deque, local_bottom, deque->age.fields.top) != GC_DEQUE_SIZE_MASK(),
              "bottom == (top - 1)\n");
    return FALSE;
}

static stack_page_t *
stack_page_alloc(void)
{
    stack_page_t *res;

    res = malloc(sizeof(stack_page_t));
    /* TODO: it's 4MB. I'll fix it to 4KB. */
    gc_assert(sizeof(stack_page_t) == (4 * 1024 * 1024 - SIZEOF_VOIDP),
              "stack_page size is not 4KB. %d bytes.\n", (int)(sizeof(stack_page_t) - SIZEOF_VOIDP));
    if (!res)
        rb_memerror();

    return res;
}

static int
is_overflow_stask_empty(overflow_stack_t *stack)
{
    return stack->page == NULL;
}

static size_t
overflow_stask_size(overflow_stack_t *stack)
{
    if (is_overflow_stask_empty(stack)) {
        return 0;
    }
    return stack->full_page_size + stack->page_index;
}

static void
push_overflow_stack_page(overflow_stack_t *stack)
{
    stack_page_t *next;
    int empty;

    gc_assert(stack->page_index == stack->page_size, "page is not full.\n");
    if (stack->cache_size > 0) {
        next = stack->cache;
        stack->cache = stack->cache->next;
        stack->cache_size--;
    }
    else {
        next = stack_page_alloc();
    }
    empty = is_overflow_stask_empty(stack);
    next->next = stack->page;
    stack->page = next;
    stack->page_index = 0;
    if (!empty) {
        stack->full_page_size += stack->page_size;
    }
}

static void
pop_overflow_stack_page(overflow_stack_t *stack)
{
    stack_page_t *prev;

    prev = stack->page->next;
    gc_assert(stack->page_index == 0, "page is not empty.\n");
    if (stack->cache_size < OVERFLOW_STACK_PAGE_CACHE_LIMIT) {
        stack->page->next = stack->cache;
        stack->cache = stack->page;
        stack->cache_size++;
    }
    else {
        free(stack->page);
    }
    stack->page = prev;
    stack->page_index = stack->page_size;
    if (prev != NULL) {
        stack->full_page_size -= stack->page_size;
    }
}

static void
free_stack_pages(overflow_stack_t *stack)
{
    stack_page_t *page = stack->page;
    while (page != NULL) {
        free(page);
        page = page->next;
    }
}

static void
stack_page_datas_store(overflow_stack_t *stack, size_t index, void *data)
{
    switch (stack->type) {
    case DEQUE_DATA_MARKSTACK_PTR:
        ((VALUE *)stack->page->datas)[index] = (VALUE)data;
	break;
    case DEQUE_DATA_ARRAY_CONTINUE:
        ((array_continue_t *)stack->page->datas)[index] = *((array_continue_t *)data);
	break;
    default:
        fprintf(stderr, "[FATAL] stack_page_datas_store(): unknown type %d\n",
                (int)stack->type);
        rb_memerror();
    }
}

static void *
stack_page_datas_entry(overflow_stack_t *stack, size_t index)
{
    switch (stack->type) {
    case DEQUE_DATA_MARKSTACK_PTR:
        return (void *)((VALUE *)stack->page->datas)[index];
    case DEQUE_DATA_ARRAY_CONTINUE:
        return (void *)(&((array_continue_t *)stack->page->datas)[index]);
    default:
        fprintf(stderr, "[FATAL] stack_page_datas_entry(): unknown type %d\n",
                (int)stack->type);
        rb_memerror();
    }
}

static void
push_overflow_stack(overflow_stack_t *stack, void *data)
{
    if (stack->page_index == stack->page_size) {
        push_overflow_stack_page(stack);
    }
    stack_page_datas_store(stack, stack->page_index++, data);
}

static int
pop_overflow_stack(overflow_stack_t *stack, void **data)
{
    if (is_overflow_stask_empty(stack)) {
        return FALSE;
    }
    if (stack->page_index == 1) {
        *data = stack_page_datas_entry(stack, --stack->page_index);
        pop_overflow_stack_page(stack);
        return TRUE;
    }
    *data = stack_page_datas_entry(stack, --stack->page_index);
    return TRUE;
}

static inline void
push_bottom_with_overflow(rb_objspace_t *objspace, deque_t *deque, void *data)
{
    if(!push_bottom(deque, data)) {
        /* overflowed */
        gc_debug("overflowed: deque(%p)\n", deque);
        count_deque_stats(OVERFLOW);
        push_overflow_stack(&deque->overflow_stack, data);
    }
}

static int
pop_bottom_with_get_back(rb_objspace_t *objspace, deque_t *deque, void **data)
{
    if(!pop_bottom(deque, data)) {
        /* empty */
        gc_assert(is_empty_deque(deque, deque->bottom, deque->age.fields.top),
                  "not empty? %d, %d\n",
                  (int)deque->bottom, (int)deque->age.fields.top);

        if (!pop_overflow_stack(&deque->overflow_stack, data)) {
            return FALSE;
        }
    }
    return TRUE;
}

static int
pop_top(deque_t *deque, void **data)
{
    union deque_age old_age, new_age, res_age;
    size_t local_bottom;

    old_age = deque->age;
    local_bottom = deque->bottom;
    if (size_deque(deque, local_bottom, old_age.fields.top) == 0) {
        return FALSE;
    }

    *data = deque_datas_entry(deque, old_age.fields.top);
    new_age = old_age;
    new_age.fields.top = deque_increment(deque, new_age.fields.top);
    if (new_age.fields.top == 0) {
        new_age.fields.tag++;
    }

    count_deque_stats(POP_TOP);
    res_age.data = atomic_compxchg_ptr((VALUE *)&deque->age.data,
                                       (VALUE)old_age.data,
                                       (VALUE)new_age.data);
    gc_assert(raw_size_deque(deque, local_bottom, new_age.fields.top) != GC_DEQUE_SIZE_MASK(),
              "bottom == (top - 1)\n");
    if (res_age.data == old_age.data) {
        return TRUE;
    }
    return FALSE;
}

static void
alloc_global_par_markstacks(rb_objspace_t *objspace, size_t size)
{
    size_t i;
    par_markstack_t *p;

    for (i = 0; i < size; i++) {
        p = malloc(sizeof(par_markstack_t));
        if (!p) {
            return rb_memerror();
        }
        p->next = objspace->par_markstack.global_list;
        objspace->par_markstack.global_list = p;
    }
    objspace->par_markstack.length += size;
    objspace->par_markstack.freed += size;
}

static void
free_global_par_markstacks(rb_objspace_t *objspace, size_t size)
{
    size_t i;
    par_markstack_t *p;

    for (i = 0; i < size; i++) {
        p = objspace->par_markstack.global_list;
        objspace->par_markstack.global_list = p->next;
        gc_assert(p != NULL, "objspace->par_markstack.global_list is small.\n");
        free(p);
    }
    objspace->par_markstack.length -= size;
    objspace->par_markstack.freed -= size;
}

static void
alloc_local_par_markstacks(rb_objspace_t *objspace, deque_t *deque,
                            size_t size, int need_lock)
{
    size_t i;
    par_markstack_t *top, *end;
    rb_vm_t *vm = GET_VM();

    if (need_lock) {
        rb_par_worker_group_mutex_lock(vm->worker_group);
    }

    top = end = objspace->par_markstack.global_list;
    if (top == NULL) {
        alloc_global_par_markstacks(objspace,
                                    objspace->par_markstack.length);
        top = end = objspace->par_markstack.global_list;
    }
    for (i = 0; i < size; i++) {
        if (objspace->par_markstack.global_list == NULL) {
            alloc_global_par_markstacks(objspace,
                                        objspace->par_markstack.length);
            end->next = objspace->par_markstack.global_list;
        }
        end = objspace->par_markstack.global_list;
        objspace->par_markstack.global_list =
            objspace->par_markstack.global_list->next;
    }

    end->next = deque->markstack.list;
    deque->markstack.list = top;

    deque->markstack.length += size;
    deque->markstack.freed += size;
    objspace->par_markstack.freed -= size;

    if (need_lock) {
        rb_par_worker_group_mutex_unlock(vm->worker_group);
    }
}

static void
free_local_par_markstacks(rb_objspace_t *objspace, deque_t *deque,
                              size_t size, int need_lock)
{
    size_t i;
    par_markstack_t *top, *end;
    rb_vm_t *vm = GET_VM();

    if (need_lock) {
        rb_par_worker_group_mutex_lock(vm->worker_group);
    }

    top = end = deque->markstack.list;
    for (i = 1; i < size; i++) {
        end = end->next;
    }

    deque->markstack.list = end->next;
    end->next = objspace->par_markstack.global_list;
    objspace->par_markstack.global_list = top;
    objspace->par_markstack.freed += size;
    deque->markstack.length -= size;
    deque->markstack.freed -= size;

    if (objspace->par_markstack.freed > objspace->par_markstack.length * 0.8) {
        free_global_par_markstacks(objspace,
                                   objspace->par_markstack.length/2);
    }

    if (need_lock) {
        rb_par_worker_group_mutex_unlock(vm->worker_group);
    }
}

static inline void
push_local_markstack(rb_objspace_t *objspace, deque_t *deque, VALUE obj)
{
    par_markstack_t *m;

    m = deque->markstack.list;
    if (deque->markstack.index >= GC_PAR_MARKSTACK_OBJS_SIZE) {
        deque->markstack.list = m->next;
        m->next = NULL;
        deque->markstack.freed--;
        push_bottom_with_overflow(objspace, deque, (void *)m);
        if (deque->markstack.max_freed > deque->markstack.freed) {
            deque->markstack.max_freed = deque->markstack.freed;
        }
        if (deque->markstack.list == NULL) {
            alloc_local_par_markstacks(objspace, deque,
                                       deque->markstack.length,
                                       TRUE);
        }
        deque->markstack.index = 0;
        m = deque->markstack.list;
    }

    m->objs[deque->markstack.index] = obj;
    deque->markstack.index++;
}

static inline void
add_local_markstack(deque_t *deque, par_markstack_t *m)
{
    m->next = deque->markstack.list;
    deque->markstack.list = m;
    deque->markstack.freed++;
    deque->markstack.index = GC_PAR_MARKSTACK_OBJS_SIZE;
}

static inline int
pop_local_markstack(rb_objspace_t *objspace, deque_t *deque, VALUE *obj)
{
    par_markstack_t *m;

    m = deque->markstack.list;
    if (deque->markstack.index == 0) {
        if (!pop_bottom_with_get_back(objspace, deque, (void **)&m)) {
            return FALSE;
        }
        add_local_markstack(deque, m);
    }

    *obj = m->objs[--deque->markstack.index];
    return TRUE;
}

static int
steal(rb_objspace_t *objspace, deque_t *deques,
      size_t deque_index, void **data)
{
    size_t c1, c2, sz1, sz2, i = 0;

    if (objspace->par_mark.num_workers > 2) {
        c1 = deque_index;
        while (c1 == deque_index) {
            c1 = rand() % objspace->par_mark.num_workers;
        }
        c2 = deque_index;
        while (c2 == deque_index) {
            c2 = rand() % objspace->par_mark.num_workers;
        }
        sz1 = size_deque(&deques[c1], deques[c1].bottom, deques[c1].age.fields.top);
        sz2 = size_deque(&deques[c2], deques[c2].bottom, deques[c2].age.fields.top);
        if (sz1 > sz2) {
            return pop_top(&deques[c1], data);
        }
        else {
            return pop_top(&deques[c2], data);
        }
    }
    else if (objspace->par_mark.num_workers == 2) {
        i = (deque_index + 1) % 2;
        return pop_top(&deques[i], data);
    }
    else {
        gc_assert(FALSE, "should not call this function.\n");
        return FALSE;
    }
}

static void
init_par_gc(rb_objspace_t *objspace)
{
    void *p;
    size_t i;
    rb_gc_par_worker_t *workers;
    size_t num_workers;
    rb_vm_t *vm = GET_VM();

    objspace->par_mark.num_workers = num_workers = initial_par_gc_threads;
    if (num_workers < 1) {
        gc_debug("init_par_mark cancel.\n");
        return;
    }

    alloc_global_par_markstacks(objspace, GC_INIT_PAR_MARKSTACK_SIZE);

    p = malloc(sizeof(deque_t) * num_workers);
    if (!p) {
        return rb_memerror();
    }
    objspace->par_mark.deques = (deque_t *)p;
    MEMZERO((void*)p, deque_t, num_workers);

    objspace->par_markstack.local_free_min =
        GC_INIT_PAR_MARKSTACK_SIZE / 2 / num_workers;
    for (i = 0; i < num_workers; i++) {
        p = malloc(GC_MSTACK_PTR_DEQUE_SIZE * sizeof(VALUE));
        if (!p) {
            return rb_memerror();
        }
        objspace->par_mark.deques[i].datas = p;
        objspace->par_mark.deques[i].size = GC_MSTACK_PTR_DEQUE_SIZE;
        objspace->par_mark.deques[i].type = DEQUE_DATA_MARKSTACK_PTR;
        objspace->par_mark.deques[i].overflow_stack.type = DEQUE_DATA_MARKSTACK_PTR;
        objspace->par_mark.deques[i].overflow_stack.page_size = PAGE_DATAS_SIZE;
        /* for alloc new page at push_overflow_stack() */
        objspace->par_mark.deques[i].overflow_stack.page_index = PAGE_DATAS_SIZE;

        alloc_local_par_markstacks(objspace, &objspace->par_mark.deques[i],
                                   objspace->par_markstack.local_free_min,
                                   FALSE);
    }

    p = malloc(sizeof(deque_t) * num_workers);
    if (!p) {
        return rb_memerror();
    }
    objspace->par_mark.array_continue_deques = (deque_t *)p;
    MEMZERO((void*)p, deque_t, num_workers);

    for (i = 0; i < num_workers; i++) {
        p = malloc(GC_ARRAY_CONTINUE_DEQUE_SIZE * sizeof(array_continue_t));
        if (!p) {
            return rb_memerror();
        }
        MEMZERO((void*)p, deque_t, 1);
        objspace->par_mark.array_continue_deques[i].datas = p;
        objspace->par_mark.array_continue_deques[i].size = GC_ARRAY_CONTINUE_DEQUE_SIZE;
        objspace->par_mark.array_continue_deques[i].type = DEQUE_DATA_ARRAY_CONTINUE;
        objspace->par_mark.array_continue_deques[i].overflow_stack.type = DEQUE_DATA_ARRAY_CONTINUE;;
        objspace->par_mark.array_continue_deques[i].overflow_stack.page_size =
            PAGE_DATAS_SIZE / (sizeof(array_continue_t) / SIZEOF_VOIDP);
        objspace->par_mark.array_continue_deques[i].overflow_stack.page_index =
            objspace->par_mark.array_continue_deques[i].overflow_stack.page_size;

    }

    p = malloc(sizeof(rb_gc_par_worker_t) * num_workers);
    if (!p) {
        return rb_memerror();
    }
    MEMZERO(p, rb_gc_par_worker_t, num_workers);
    workers = (rb_gc_par_worker_t *)p;
    for (i = 0; i < num_workers; i++) {
        workers[i].local_deque = &objspace->par_mark.deques[i];
        workers[i].local_array_conts = &objspace->par_mark.array_continue_deques[i];
        workers[i].index = i;
    }

    vm->worker_group = rb_gc_par_worker_group_create(num_workers, workers);
    if (!vm->worker_group) {
        return rb_memerror();
    }
}

static void
push_array_continue(rb_objspace_t *objspace, deque_t *array_conts,
                    RVALUE *obj, size_t index)
{
    array_continue_t ac;

    ac.obj = obj;
    ac.index = index;
    gc_assert(ac.index <= (size_t)RARRAY_LEN(obj), "too big.\n");
    push_bottom_with_overflow(objspace, array_conts, (void *)&ac);
}

static void
par_mark_array_object(rb_objspace_t *objspace, rb_gc_par_worker_t *worker,
                      RVALUE *obj, size_t index)
{
    size_t end;
    size_t len = RARRAY_LEN(obj);
    VALUE *ptr = RARRAY_PTR(obj);

    if ((index + GC_ARRAY_CONTINUE_DEQUE_STRIDE) >= len) {
        end = len;
    }
    else {
        end = index + GC_ARRAY_CONTINUE_DEQUE_STRIDE;
    }

    for(; index < end; index++) {
        gc_mark(objspace, *(ptr+index), 0, worker);
    }
    if (end < len) {
        push_array_continue(objspace, worker->local_array_conts, obj, index);
    }
}


static void
gc_follow_marking_deques(rb_objspace_t *objspace, rb_gc_par_worker_t *worker)
{
    VALUE p;
    array_continue_t *ac;
    int deque_empty = FALSE;

    do {
        while(pop_local_markstack(objspace, worker->local_deque, (VALUE *)&p)) {
            gc_mark_children(objspace, p, 0, worker);
        }

        while(pop_bottom_with_get_back(objspace, worker->local_array_conts, (void **)&ac)) {
            par_mark_array_object(objspace, worker, ac->obj, ac->index);
        }

        deque_empty = is_empty_deque(worker->local_deque, worker->local_deque->bottom, worker->local_deque->age.fields.top) &&
            is_empty_deque(worker->local_array_conts, worker->local_array_conts->bottom, worker->local_array_conts->age.fields.top);
    } while (!deque_empty);
}

static void
steal_mark_task(rb_gc_par_worker_t *worker)
{
    par_markstack_t *m;
    array_continue_t *ac;
    rb_objspace_t *objspace = &rb_objspace;

    do {
        while (steal(objspace, objspace->par_mark.array_continue_deques,
                     worker->index, (void **)&ac)) {
            par_mark_array_object(objspace, worker, ac->obj, ac->index);
            gc_follow_marking_deques(objspace, worker);
        }

        while (steal(objspace, objspace->par_mark.deques,
                     worker->index, (void **)&m)) {
            add_local_markstack(worker->local_deque, m);
            gc_follow_marking_deques(objspace, worker);
        }
    } while (!rb_par_steal_task_offer_termination(worker->group));

    if (worker->local_deque->markstack.max_freed >
        objspace->par_markstack.local_free_min
        && worker->local_deque->markstack.max_freed >
        (worker->local_deque->markstack.freed * 0.8)) {
        free_local_par_markstacks(objspace, worker->local_deque,
                                  worker->local_deque->markstack.freed / 2,
                                  TRUE);
    }
}

#ifdef GC_DEBUG

static rb_gc_par_worker_t* current_gc_worker(void);

void
gc_par_print_test(rb_gc_par_worker_t *worker)
{
    gc_assert(worker == current_gc_worker(), "not eq\n");
    gc_debug("other thread! %p\n", worker);
}

VALUE
rb_gc_test(void)
{
    rb_objspace_t *objspace = &rb_objspace;
    deque_t *deque = &objspace->par_mark.deques[0];
    size_t res, tmp_num_workers, i;
    VALUE data, ary;
    array_continue_t ac;
    array_continue_t *res_ac;
    rb_gc_par_worker_t *worker;
    rb_gc_par_worker_t *workers = GET_VM()->worker_group->workers;
    void (*tasks[10]) (rb_gc_par_worker_t *worker);

    objspace->par_mark.deques[1].bottom = 0;
    objspace->par_mark.deques[1].age.data = 0;
    objspace->par_mark.deques[2].bottom = 0;
    objspace->par_mark.deques[2].age.data = 0;
    objspace->par_mark.deques[3].bottom = 0;
    objspace->par_mark.deques[3].age.data = 0;

    printf("deque size test\n");
    deque->age.fields.top = GC_DEQUE_SIZE_MASK();
    deque->age.fields.top = deque_increment(deque, deque->age.fields.top);
    gc_assert(deque->age.fields.top == 0, "%d\n", deque->age.fields.top);

    deque->age.fields.top = 0;
    deque->age.fields.top = deque_decrement(deque, deque->age.fields.top);
    gc_assert(deque->age.fields.top == GC_DEQUE_SIZE_MASK(), "%d %d\n",
              (int)deque->age.fields.top, (int)GC_DEQUE_SIZE_MASK());

    deque->age.fields.top = 0;
    deque->bottom = 0;
    gc_assert(size_deque(deque, deque->bottom, deque->age.fields.top) == 0, "not eq\n");

    deque->age.fields.top = 1;
    deque->bottom = 0;
    gc_assert(size_deque(deque, deque->bottom, deque->age.fields.top) == 0, "not eq\n");

    deque->age.fields.top = 2;
    deque->bottom = 0;
    gc_assert(size_deque(deque, deque->bottom, deque->age.fields.top) == GC_DEQUE_MAX(), "not eq\n");


    printf("is_full_deque test\n");
    deque->age.fields.top = 0;
    deque->bottom = GC_DEQUE_MAX();
    gc_assert(is_full_deque(deque, deque->bottom, deque->age.fields.top),
              "size: %d\n",
              (int)size_deque(deque, deque->bottom, deque->age.fields.top));

    deque->age.fields.top = 1;
    deque->bottom = 0;
    gc_assert(!is_full_deque(deque, deque->bottom, deque->age.fields.top),
              "size: %d\n",
              (int)size_deque(deque, deque->bottom, deque->age.fields.top));

    deque->age.fields.top = 0;
    deque->bottom = GC_DEQUE_MAX() - 1;
    gc_assert(!is_full_deque(deque, deque->bottom, deque->age.fields.top),
              "size: %d\n",
              (int)size_deque(deque, deque->bottom, deque->age.fields.top));


    printf("push_bottom test\n");
    deque->age.fields.top = 0;
    deque->bottom = 0;
    res = push_bottom(deque, (void *)1);
    gc_assert(res, "false?");
    gc_assert((VALUE)deque->datas[0] == 1, "datas[0] %p\n",
              (void *)deque->datas[0]);
    gc_assert((int)deque->bottom == 1, "bottom %d\n",
              (int)deque->bottom);

    res = push_bottom(deque, (void *)2);
    gc_assert(res, "false?");
    gc_assert((VALUE)deque->datas[1] == 2, "datas[1] %p\n",
              (void *)deque->datas[1]);

    res = push_bottom(deque, (void *)2);
    gc_assert(res, "false?");
    gc_assert((VALUE)deque->datas[1] == 2, "datas[1] %p\n",
              (void *)deque->datas[1]);

    deque->age.fields.top = 0;
    deque->bottom = GC_DEQUE_MAX();
    res = push_bottom(deque, (void *)2);
    gc_assert(!res, "true?");

    deque->age.fields.top = 0;
    deque->bottom = GC_DEQUE_SIZE_MASK();
    res = push_bottom(deque, (void *)2);
    gc_assert(res, "false?");

    deque->age.fields.top = 5;
    deque->bottom = 2;
    res = push_bottom(deque, (void *)2);
    gc_assert(res, "false?");
    gc_assert(deque->bottom == 3, "bottom %d\n", (int)deque->bottom);
    gc_assert((VALUE)deque->datas[2] == 2, "datas[3] %p\n",
              (void *)deque->datas[2]);

    res = push_bottom(deque, (void *)2);
    gc_assert(!res, "true?");
    gc_assert(deque->bottom == 3, "bottom %d\n", (int)deque->bottom);


    printf("is_empty\n");
    deque->age.fields.top = 0;
    deque->bottom = 0;
    res = is_empty_deque(deque, deque->bottom, deque->age.fields.top);
    gc_assert(res == TRUE, "res %d\n", (int)res);

    deque->bottom = 1;
    res = is_empty_deque(deque, deque->bottom, deque->age.fields.top);
    gc_assert(res == FALSE, "res %d\n", (int)res);

    deque->age.fields.top = 0;
    deque->bottom = 0;
    push_local_markstack(objspace, deque, (VALUE)2);
    res = is_empty_deque(deque, deque->bottom, deque->age.fields.top);
    gc_assert(res == FALSE, "res %d\n", (int)res);

    pop_local_markstack(objspace, deque, (VALUE *)&data);
    res = is_empty_deque(deque, deque->bottom, deque->age.fields.top);
    gc_assert(res == TRUE, "res %d\n", (int)res);

    printf("order_access_memory_barrier\n");
    order_access_memory_barrier();

    printf("pop_bottom test\n");
    deque->age.fields.top = 0;
    deque->bottom = 0;
    push_bottom(deque, (void *)1);
    push_bottom(deque, (void *)2);
    res = pop_bottom(deque, (void **)&data);
    gc_assert(res == TRUE, "fail\n");
    gc_assert(data == 2, "data %d\n", (int)data);

    res = pop_bottom(deque, (void **)&data);
    /* pop_bottom win */
    gc_assert(res == TRUE, "fail\n");
    gc_assert(data == 1, "data %d\n", (int)data);
    gc_assert(deque->age.fields.top == 0, "top %d\n", deque->age.fields.top);
    gc_assert(deque->age.fields.tag == 1, "tag %d\n", deque->age.fields.tag);
    /* pop_bottom lose
    gc_assert(res == FALSE, "fail\n");
    */
    res = pop_bottom(deque, (void **)&data);
    data = 2;
    gc_assert(res == FALSE, "fail\n");
    gc_assert(data == 2, "data %d\n", (int)data);

    deque->age.fields.top = 5;
    deque->bottom = 2;
    push_bottom(deque, (void *)3);
    res = pop_bottom(deque, (void **)&data);
    gc_assert(res == TRUE, "fail\n");
    gc_assert(data == 3, "data %d\n", (int)data);
    gc_assert(deque->bottom == 2, "bottom %d\n", (int)deque->bottom);
    pop_bottom(deque, (void **)&data);
    pop_bottom(deque, (void **)&data);
    pop_bottom(deque, (void **)&data);
    gc_assert(deque->bottom == GC_DEQUE_SIZE_MASK(), "bottom %d\n",
              (int)deque->bottom);

    printf("par_mark_array_object\n");
    ary = rb_ary_new2(GC_ARRAY_CONTINUE_DEQUE_STRIDE*2);
    rb_ary_store(ary, GC_ARRAY_CONTINUE_DEQUE_STRIDE*2-1, Qtrue);
    par_mark_array_object(objspace, &workers[0], (RVALUE *)ary, 0);
    ac = ((array_continue_t *)(workers[0].local_array_conts->datas))[0];
    gc_assert((VALUE)ac.obj == ary, "not eq\n");
    gc_assert(ac.index == 512, "not eq\n");
    par_mark_array_object(objspace, &workers[0], (RVALUE *)ary, 512);
    ac = ((array_continue_t *)(workers[0].local_array_conts->datas))[1];
    gc_assert((VALUE)ac.obj != ary, "not eq\n");
    par_mark_array_object(objspace, &workers[0], (RVALUE *)ary, 510);
    ac = ((array_continue_t *)(workers[0].local_array_conts->datas))[1];
    gc_assert((VALUE)ac.obj == ary, "not eq\n");
    gc_assert(ac.index == 1022, "not eq\n");

    printf("pop_top test\n");
    deque->age.fields.top = 0;
    deque->age.fields.tag = 0;
    deque->bottom = 0;
    push_bottom(deque, (void *)1);
    res = pop_top(deque, (void **)&data);
    gc_assert(res == TRUE, "fail\n");
    gc_assert(data == 1, "data %d\n", (int)data);
    gc_assert(deque->age.fields.top == 1, "top %d\n", deque->age.fields.top);
    gc_assert(deque->age.fields.tag == 0, "tag %d\n", deque->age.fields.tag);

    res = pop_top(deque, (void **)&data);
    gc_assert(res == FALSE, "fail\n");
    gc_assert(deque->age.fields.top == 1, "top %d\n", deque->age.fields.top);

    deque->age.fields.top = GC_DEQUE_SIZE_MASK();
    deque->bottom = 2;
    res = pop_top(deque, (void **)&data);
    gc_assert(res == TRUE, "fail\n");
    gc_assert(deque->age.fields.top == 0, "top %d\n", deque->age.fields.top);
    gc_assert(deque->age.fields.tag == 1, "tag %d\n", deque->age.fields.tag);

    res = pop_top(workers[0].local_array_conts, (void **)&res_ac);
    gc_assert(res == TRUE, "fail\n");
    gc_assert((VALUE)res_ac->obj == ary, "ac.ary = %p\n", res_ac->obj);
    gc_assert(res_ac->index == 512, "ac.index = %d\n", (int)res_ac->index);

    printf("push_overflow_stack\n");
    deque->age.fields.top = 0;
    deque->bottom = 0;
    for (i = 0; i <= 4; i++) {
        push_overflow_stack(&deque->overflow_stack, (void *)1);
        gc_assert((VALUE)deque->overflow_stack.page->datas[0] == 1, "not 1\n");
        gc_assert(deque->overflow_stack.page_index == 1, "not 1\n");
        gc_assert(deque->overflow_stack.cache_size == 0, "invalid cache_size %d(%d)\n",
                  (int)deque->overflow_stack.cache_size, (int)i);
        deque->overflow_stack.page_index = PAGE_DATAS_SIZE;
    }
    gc_assert(overflow_stask_size(&deque->overflow_stack) == PAGE_DATAS_SIZE*5,
              "size %d\n", (int)overflow_stask_size(&deque->overflow_stack));

    printf("pop_overflow_stack\n");
    for (i = 1; i <= 4; i++) {
        deque->overflow_stack.page_index = 1;
        res = pop_overflow_stack(&deque->overflow_stack, (void **)&data);
        gc_assert(res == TRUE, "false?\n");
        gc_assert((int)data == 1, "(%d)\n", (int)data);
        gc_assert(deque->overflow_stack.page_index == PAGE_DATAS_SIZE, "invalid size\n");
        gc_assert(deque->overflow_stack.cache_size == i, "invalid cache_size\n");
    }
    deque->overflow_stack.page_index = 1;
    res = pop_overflow_stack(&deque->overflow_stack, (void **)&data);
    gc_assert(res == TRUE, "false?\n");
    gc_assert(deque->overflow_stack.cache_size == 4, "invalid cache_size %d(%d)\n",
                  (int)deque->overflow_stack.cache_size, (int)i);
    res = pop_overflow_stack(&deque->overflow_stack, (void **)&data);
    gc_assert(res == FALSE, "true?\n");
    gc_assert(overflow_stask_size(&deque->overflow_stack) == 0,
              "invalid stack size\n");

    printf("push_bottom_with_overflow\n");
    deque->age.fields.top = 0;
    deque->bottom = 0;
    push_bottom_with_overflow(objspace, deque, (void *)1);
    gc_assert((VALUE)deque->datas[0] == 1, "eq\n");

    deque->age.fields.top = 0;
    deque->bottom = 0;
    for (i = 0; i < GC_DEQUE_MAX(); i++) {
        push_bottom(deque, (void *)(i+1));
    }
    push_bottom_with_overflow(objspace, deque, (void *)2);
    gc_assert((VALUE)deque->overflow_stack.page->datas[0] == 2,
              "data %p\n", deque->overflow_stack.page->datas[0]);
    gc_assert(deque->overflow_stack.page_index == 1, "not 1\n");
    gc_assert(deque->overflow_stack.cache_size == 3, "cache_size %d\n",
              (int)deque->overflow_stack.cache_size);
    deque->age.fields.top = 0;
    deque->bottom = 0;

    {
        par_markstack_t *tmp;
        int diff;

        printf("alloc_global_par_markstacks\n");
        diff = objspace->par_markstack.freed;
        alloc_global_par_markstacks(objspace, 10);
        i = 0;
        tmp = objspace->par_markstack.global_list;
        while (tmp != NULL) {
            tmp = tmp->next;
            i++;
        }
        diff = objspace->par_markstack.freed - diff;
        gc_assert(10 == diff, "diff(%d)\n", diff);
    }

    {
        par_markstack_t *tmp;
        int diff;

        printf("free_global_par_markstacks\n");
        diff = objspace->par_markstack.freed;
        free_global_par_markstacks(objspace, 10);
        i = 0;
        tmp = objspace->par_markstack.global_list;
        while (tmp != NULL) {
            tmp = tmp->next;
            i++;
        }
        diff = objspace->par_markstack.freed - diff;
        gc_assert(-10 == diff, "diff(%d)\n", diff);
    }

    {
        par_markstack_t *tmp;
        int diff;

        printf("alloc_local_par_markstacks\n");
        diff = objspace->par_markstack.freed;
        alloc_local_par_markstacks(objspace, deque, 10, FALSE);
        i = 0;
        tmp = objspace->par_markstack.global_list;
        while (tmp != NULL) {
            tmp = tmp->next;
            i++;
        }
        diff = diff - i;
        gc_assert(10 == diff, "diff(%d)\n", diff);
    }

    {
        par_markstack_t *tmp;
        int diff;

        printf("free_local_par_markstacks\n");
        diff = objspace->par_markstack.freed;
        free_local_par_markstacks(objspace, deque, 10, FALSE);
        i = 0;
        tmp = objspace->par_markstack.global_list;
        while (tmp != NULL) {
            tmp = tmp->next;
            i++;
        }
        diff = diff - i;
        gc_assert(-10 == diff, "diff(%d)\n", diff);
    }

    printf("pop_bottom_with_get_back\n");
    push_bottom(deque, (void *)1);
    res = pop_bottom_with_get_back(objspace, deque, (void **)&data);
    gc_assert(res == TRUE, "false?\n");
    gc_assert(data == 1, "%d\n", (int)data);
    res = pop_bottom_with_get_back(objspace, deque, (void **)&data);
    gc_assert(res == TRUE, "false?\n");
    gc_assert(data == 2, "%d\n", (int)data);
    gc_assert(deque->overflow_stack.full_page_size == 0, "not zero\n");
    gc_assert(deque->overflow_stack.page_index == PAGE_DATAS_SIZE, "not zero\n");
    gc_assert(deque->overflow_stack.cache_size == 4, "not 1\n");
    deque->age.fields.top = 0;
    deque->bottom = 0;
    res = pop_bottom_with_get_back(objspace, deque, (void **)&data);
    gc_assert(res == FALSE, "false?\n");

    printf("push_local_markstack\n");
    deque->markstack.max_freed = deque->markstack.freed;
    push_local_markstack(objspace, deque, (VALUE)1);
    gc_assert(deque->markstack.list->objs[0] == (VALUE)1, "not 1\n");
    gc_assert(deque->markstack.index == 1, "not 1\n");
    deque->markstack.index = GC_PAR_MARKSTACK_OBJS_SIZE;
    push_local_markstack(objspace, deque, (VALUE)2);
    gc_assert(deque->markstack.list->objs[0] == 2, "not 2\n");
    gc_assert(deque->markstack.index == 1, "not 1\n");
    gc_assert(deque->markstack.freed == objspace->par_markstack.local_free_min-1,
              "%d\n", (int)deque->markstack.freed);
    gc_assert(deque->markstack.max_freed == deque->markstack.freed,
              "%d == %d\n", (int)deque->markstack.max_freed,
              (int)deque->markstack.freed);
    gc_assert(deque->markstack.length == objspace->par_markstack.local_free_min,
              "%d\n", (int)deque->markstack.length);

    printf("pop_local_markstack\n");
    res = pop_local_markstack(objspace, deque, (VALUE *)&data);
    gc_assert(res == TRUE, "false\n");
    gc_assert(data == 2, "not 2\n");
    gc_assert(deque->markstack.index == 0, "not 0\n");
    res = pop_local_markstack(objspace, deque, (VALUE *)&data);
    gc_assert(res == TRUE, "false\n");
    gc_assert(data == 0, "data: %d\n", (int)data);
    gc_assert(deque->markstack.index == GC_PAR_MARKSTACK_OBJS_SIZE-1,
              "not eq\n");
    gc_assert(deque->markstack.max_freed == deque->markstack.length-1,
              "%d != %d\n",
              (int)deque->markstack.max_freed, (int)deque->markstack.length-1);
    deque->markstack.index = 0;
    res = pop_local_markstack(objspace, deque, (VALUE *)&data);
    gc_assert(res == FALSE, "false\n");
    gc_assert(deque->markstack.freed == objspace->par_markstack.local_free_min,
              "%d\n", (int)deque->markstack.freed);
    gc_assert(deque->markstack.length == objspace->par_markstack.local_free_min,
              "%d\n", (int)deque->markstack.length);

    printf("worker\n");
    worker = &workers[objspace->par_mark.num_workers-1];
    gc_assert(worker->index == objspace->par_mark.num_workers-1,
              "index(%d)?\n", (int)worker->index);
    worker = &workers[0];
    gc_assert(worker->index == 0, "?\n");

    printf("run task\n");
    gc_debug("objspace->par_mark.num_workers(%d)\n",
             (int)objspace->par_mark.num_workers);
    tasks[0] = gc_par_print_test;
    tasks[1] = gc_par_print_test;
    tasks[2] = gc_par_print_test;
    rb_gc_par_worker_group_run_tasks(worker->group, tasks, 3);

    printf("steal. use randome, so lack of stability test case.\n");
    tmp_num_workers = objspace->par_mark.num_workers;
    objspace->par_mark.num_workers = 4;
    deque->bottom = 0;
    deque->age.data = 0;
    push_bottom(&objspace->par_mark.deques[1], (void *)11);
    push_bottom(&objspace->par_mark.deques[1], (void *)12);
    push_bottom(&objspace->par_mark.deques[1], (void *)13);
    push_bottom(&objspace->par_mark.deques[2], (void *)21);
    push_bottom(&objspace->par_mark.deques[2], (void *)22);
    push_bottom(&objspace->par_mark.deques[3], (void *)31);
    res = steal(objspace, objspace->par_mark.deques, 0, (void **)&data);
    gc_assert(res == TRUE, "res: %d\n", (int)res);
    gc_assert(data != 0, "data: %d\n", (int)data);
    res = steal(objspace, objspace->par_mark.deques, 0, (void **)&data);
    gc_assert(res == TRUE, "res: %d\n", (int)res);
    gc_assert(data != 0, "data: %d\n", (int)data);
    res = steal(objspace, objspace->par_mark.deques, 0, (void **)&data);
    gc_assert(res == TRUE, "res: %d\n", (int)res);
    gc_assert(data != 0, "data: %d\n", (int)data);
    res = steal(objspace, objspace->par_mark.deques, 0, (void **)&data);
    gc_assert(res == TRUE, "res: %d\n", (int)res);
    gc_assert(data != 0, "data: %d\n", (int)data);
    res = steal(objspace, objspace->par_mark.deques, 0, (void **)&data);
    gc_assert(res == TRUE, "res: %d\n", (int)res);
    gc_assert(data != 0, "data: %d\n", (int)data);
    res = steal(objspace, objspace->par_mark.deques, 0, (void **)&data);
    gc_assert(res == TRUE, "res: %d\n", (int)res);
    gc_assert(data != 0, "data: %d\n", (int)data);
    res = steal(objspace, objspace->par_mark.deques, 0, (void **)&data);
    gc_assert(res == 0, "res: %d\n", (int)res);
    objspace->par_mark.num_workers = tmp_num_workers;

    push_bottom(&objspace->par_mark.deques[0], (void *)1);
    tmp_num_workers = objspace->par_mark.num_workers;
    objspace->par_mark.num_workers = 2;
    res = steal(objspace, objspace->par_mark.deques, 1, (void **)&data);
    gc_assert(res == TRUE, "res: %d\n", (int)res);
    gc_assert(data == 1, "data: %d\n", (int)data);
    push_bottom(&objspace->par_mark.deques[1], (void *)11);
    res = steal(objspace, objspace->par_mark.deques, 0, (void **)&data);
    gc_assert(data == 11, "data: %d\n", (int)data);
    res = steal(objspace, objspace->par_mark.deques, 0, (void **)&data);
    gc_assert(res == 0, "res: %d\n", (int)res);
    objspace->par_mark.num_workers = tmp_num_workers;

    res = steal(objspace, objspace->par_mark.array_continue_deques,
                1, (void **)&res_ac);
    gc_assert(res == TRUE, "res: %d\n", (int)res);
    gc_assert((VALUE)res_ac->obj == ary, "ac.obj: %p\n", res_ac->obj);
    gc_assert((int)res_ac->index == 1022, "ac.index: %d\n", (int)res_ac->index);

    printf("all test passed\n");
    return Qnil;
}

#endif /* GC_DEBUG*/
