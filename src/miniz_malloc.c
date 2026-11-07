#include "ktuyome.h"
#include <stddef.h>
void *malloc(size_t n){ return kmalloc((u32)n); }
void free(void *p){ kfree(p); }
void *realloc(void *p, size_t n){ return krealloc(p, (u32)n); }