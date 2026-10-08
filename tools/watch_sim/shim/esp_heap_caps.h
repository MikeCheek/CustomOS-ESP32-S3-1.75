#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
#define MALLOC_CAP_DMA 8
#define MALLOC_CAP_32BIT 16
#define MALLOC_CAP_DEFAULT 32
static inline void *heap_caps_malloc(size_t n, unsigned) { return malloc(n); }
static inline void *heap_caps_calloc(size_t n, size_t m, unsigned) { return calloc(n, m); }
static inline void *heap_caps_realloc(void *p, size_t n, unsigned) { return realloc(p, n); }
static inline void *heap_caps_aligned_alloc(size_t, size_t n, unsigned) { return malloc(n); }
static inline void heap_caps_free(void *p) { free(p); }
static inline size_t heap_caps_get_free_size(unsigned) { return 4 * 1024 * 1024; }
static inline size_t heap_caps_get_largest_free_block(unsigned) { return 4 * 1024 * 1024; }
static inline size_t heap_caps_get_minimum_free_size(unsigned) { return 2 * 1024 * 1024; }
static inline size_t heap_caps_get_total_size(unsigned) { return 8 * 1024 * 1024; }
