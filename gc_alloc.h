#ifndef RUBY_GC_ALLOC_H
#define RUBY_GC_ALLOC_H

void *ruby_xmalloc(size_t);
void *ruby_xmalloc2(size_t, size_t);
void *ruby_xcalloc(size_t, size_t);
void *ruby_xrealloc(void *, size_t);
void *ruby_xrealloc2(void *, size_t, size_t);
void ruby_xfree(void *);
void *ruby_mimmalloc(size_t);

#endif
