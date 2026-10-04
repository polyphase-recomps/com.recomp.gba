/* agb runtime: the C library subset GBA decomps link from libc.a. */
#ifndef AGB_COMPAT_STDLIB_H
#define AGB_COMPAT_STDLIB_H
#include <stddef.h>
int abs(int v);
long labs(long v);
int rand(void);
void srand(unsigned int seed);
void *malloc(size_t n);
void free(void *p);
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
#endif
