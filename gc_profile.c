/* for GC profile */
#define GC_PROFILE_MORE_DETAIL 0
#define GC_WORKER_PROFILE 1

typedef struct gc_profile_record {
    double gc_time;
    double gc_mark_time;
    double gc_sweep_time;
    double gc_invoke_time;
#if GC_WORKER_PROFILE
    double gc_worker_wakeup_time;
    double gc_worker_times[20];
#endif

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

static double
gettimeofday_time(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec * 1e-6;
}

#define GC_PROF_TIMER_START do {\
	if (objspace->profile.run) {\
	    if (!objspace->profile.record) {\
		objspace->profile.size = 1000;\
		objspace->profile.record = malloc(sizeof(gc_profile_record) * objspace->profile.size);\
	    }\
	    if (count >= objspace->profile.size) {\
		objspace->profile.size += 1000;\
		objspace->profile.record = realloc(objspace->profile.record, sizeof(gc_profile_record) * objspace->profile.size);\
	    }\
	    if (!objspace->profile.record) {\
		rb_bug("gc_profile malloc or realloc miss");\
	    }\
	    MEMZERO(&objspace->profile.record[count], gc_profile_record, 1);\
	    gc_time = gettimeofday_time();\
	    objspace->profile.record[count].gc_invoke_time = gc_time - objspace->profile.invoke_time;\
	}\
    } while(0)

#define GC_PROF_TIMER_STOP(marked) do {\
	if (objspace->profile.run) {\
	    gc_time = gettimeofday_time() - gc_time;\
	    if (gc_time < 0) gc_time = 0;\
	    objspace->profile.record[count].gc_time = gc_time;\
	    objspace->profile.record[count].is_marked = !!(marked);\
	    GC_PROF_SET_HEAP_INFO(objspace->profile.record[count]);\
	    objspace->profile.count++;\
	}\
    } while(0)

#if GC_PROFILE_MORE_DETAIL
#define INIT_GC_PROF_PARAMS double gc_time = 0, sweep_time = 0;\
    size_t count = objspace->profile.count, total = 0, live = 0

#define GC_PROF_MARK_TIMER_START double mark_time = 0;\
    do {\
	if (objspace->profile.run) {\
	    mark_time = gettimeofday_time();\
	}\
    } while(0)

#define GC_PROF_MARK_TIMER_STOP do {\
	if (objspace->profile.run) {\
	    mark_time = gettimeofday_time() - mark_time;\
	    if (mark_time < 0) mark_time = 0;\
	    objspace->profile.record[objspace->profile.count].gc_mark_time = mark_time;\
	}\
    } while(0)

#define GC_PROF_SWEEP_TIMER_START do {\
	if (objspace->profile.run) {\
	    sweep_time = gettimeofday_time();\
	}\
    } while(0)

#define GC_PROF_SWEEP_TIMER_STOP do {\
	if (objspace->profile.run) {\
	    sweep_time = gettimeofday_time() - sweep_time;\
	    if (sweep_time < 0) sweep_time = 0;\
	    objspace->profile.record[count].gc_sweep_time = sweep_time;\
	}\
    } while(0)
#define GC_PROF_SET_MALLOC_INFO do {\
	if (objspace->profile.run) {\
	    gc_profile_record *record = &objspace->profile.record[objspace->profile.count];\
	    record->allocate_increase = malloc_increase;\
	    record->allocate_limit = malloc_limit; \
	}\
    } while(0)
#define GC_PROF_SET_HEAP_INFO(record) do {\
        live = objspace->heap.live_num;\
        total = heaps_used * HEAP_OBJ_LIMIT;\
        (record).heap_use_slots = heaps_used;\
        (record).heap_live_objects = live;\
        (record).heap_free_objects = total - live;\
        (record).heap_total_objects = total;\
        (record).have_finalize = deferred_final_list ? Qtrue : Qfalse;\
        (record).heap_use_size = live * sizeof(RVALUE);\
        (record).heap_total_size = total * sizeof(RVALUE);\
    } while(0)
#define GC_PROF_INC_LIVE_NUM objspace->heap.live_num++
#define GC_PROF_DEC_LIVE_NUM objspace->heap.live_num--
#else
#define INIT_GC_PROF_PARAMS double gc_time = 0;\
    size_t count = objspace->profile.count, total = 0, live = 0
#define GC_PROF_MARK_TIMER_START
#define GC_PROF_MARK_TIMER_STOP
#define GC_PROF_SWEEP_TIMER_START
#define GC_PROF_SWEEP_TIMER_STOP
#define GC_PROF_SET_MALLOC_INFO
#define GC_PROF_SET_HEAP_INFO(record) do {\
        live = objspace->heap.live_num;\
        total = heaps_used * HEAP_OBJ_LIMIT;\
        (record).heap_total_objects = total;\
        (record).heap_use_size = live * sizeof(RVALUE);\
        (record).heap_total_size = total * sizeof(RVALUE);\
    } while(0)
#define GC_PROF_INC_LIVE_NUM
#define GC_PROF_DEC_LIVE_NUM
#endif

#if GC_WORKER_PROFILE
#define GC_PROF_WORKER_WAKEUP_START do {\
    if (objspace->profile.run) {\
        objspace->profile.record[objspace->profile.count].gc_worker_wakeup_time = gettimeofday_time();\
    }\
} while(0)
#define GC_PROF_WORKER_WAKEUP_STOP do {\
    if (objspace->profile.run) {\
    objspace->profile.record[objspace->profile.count].gc_worker_wakeup_time = gettimeofday_time() - objspace->profile.record[objspace->profile.count].gc_worker_wakeup_time;\
    }\
} while(0)

#define GC_PROF_WORKER_START(w_i) do {\
    if (objspace->profile.run && objspace->profile.record[objspace->profile.count].gc_worker_times[w_i] == 0) {\
    objspace->profile.record[objspace->profile.count].gc_worker_times[w_i] = gettimeofday_time();\
    }\
} while(0)
#define GC_PROF_WORKER_STOP(w_i) do {\
    if (objspace->profile.run) {\
    objspace->profile.record[objspace->profile.count].gc_worker_times[w_i] = gettimeofday_time() - objspace->profile.record[objspace->profile.count].gc_worker_times[w_i];\
    }\
} while(0)
#else
#define GC_PROF_WORKER_WAKEUP_START
#define GC_PROF_WORKER_WAKEUP_STOP
#define GC_PROF_WORKER_START(w_i)
#define GC_PROF_WORKER_STOP(w_i)
#endif
