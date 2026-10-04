/*
 * wasm2c runtime for the guest module (replaces wabt's wasm-rt-impl.c): one fixed
 * memory buffer as described in agbw.h,
 * function tables, traps reported through the platform host, and the exception
 * state used by setjmp/longjmp (wasm exception handling). Plain C, no threads, no
 * signals, so it builds with any host compiler (MSVC/clang, devkitPPC...).
 */
#include <stdlib.h>
#include <string.h>

#include "wasm-rt.h"
#include "wasm-rt-exceptions.h"

#include "agbw.h"
#include "agb_host.h"

#include <stdio.h>

static bool sInitialized;

void wasm_rt_init(void)
{
    sInitialized = true;
}

bool wasm_rt_is_initialized(void)
{
    return sInitialized;
}

void wasm_rt_free(void)
{
    sInitialized = false;
}

void wasm_rt_init_thread(void) {}
void wasm_rt_free_thread(void) {}

const char *wasm_rt_strerror(wasm_rt_trap_t trap)
{
    switch (trap)
    {
    case WASM_RT_TRAP_NONE: return "no error";
    case WASM_RT_TRAP_OOB: return "out-of-bounds access";
    case WASM_RT_TRAP_INT_OVERFLOW: return "integer overflow";
    case WASM_RT_TRAP_DIV_BY_ZERO: return "integer divide by zero";
    case WASM_RT_TRAP_INVALID_CONVERSION: return "invalid conversion";
    case WASM_RT_TRAP_UNREACHABLE: return "unreachable executed";
    case WASM_RT_TRAP_CALL_INDIRECT: return "invalid call_indirect";
    case WASM_RT_TRAP_UNCAUGHT_EXCEPTION: return "uncaught exception";
    case WASM_RT_TRAP_UNALIGNED: return "unaligned atomic access";
    case WASM_RT_TRAP_NULL_REF: return "null reference";
    default: return "trap";
    }
}

void agbw_report_trap(const char *what); /* agbw_guest.c */

WASM_RT_NO_RETURN void wasm_rt_trap(wasm_rt_trap_t trap)
{
    agbw_report_trap(wasm_rt_strerror(trap));
    abort();
}

void agbw_bad_indirect_call(uint32_t index)
{
    char line[96];

    snprintf(line, sizeof(line), "call through an empty function table slot (%u)", (unsigned)index);
    agb_host_log(line);
    wasm_rt_trap(WASM_RT_TRAP_CALL_INDIRECT);
}

/* ---- memory --------------------------------------------------------------------- */
void wasm_rt_allocate_memory(wasm_rt_memory_t *mem, uint64_t initial_pages, uint64_t max_pages, bool is64,
                             uint32_t page_size)
{
#if AGBW_PAGED
    /* only the writable part is allocated; the read-only data is paged (agbw_guest.c) */
    extern uint8_t *agbw_ram;

    agbw_ram = (uint8_t *)calloc(1, AGBW_ALLOC_BYTES - AGBW_RAM_BASE);
    if (agbw_ram == NULL)
    {
        agb_host_fatal("cannot allocate the guest memory");
    }
    mem->data = agbw_ram; /* not used for addressing (agbw_mem_ops.h) */
    mem->data_end = agbw_ram + (AGBW_ALLOC_BYTES - AGBW_RAM_BASE);
#else
    mem->data = (uint8_t *)calloc(1, AGBW_ALLOC_BYTES);
    if (mem->data == NULL)
    {
        agb_host_fatal("cannot allocate the guest memory");
    }
    mem->data_end = mem->data + AGBW_ALLOC_BYTES;
#endif
    mem->page_size = page_size;
    mem->pages = initial_pages;
    mem->max_pages = max_pages;
    mem->size = initial_pages * page_size;
    mem->is64 = is64;
}

uint64_t wasm_rt_grow_memory(wasm_rt_memory_t *mem, uint64_t delta)
{
    /* the layout is fixed (agbw.h): the module is linked with all its memory */
    return delta == 0 ? mem->pages : (uint64_t)-1;
}

void wasm_rt_free_memory(wasm_rt_memory_t *mem)
{
    free(mem->data);
    mem->data = mem->data_end = NULL;
}

/* ---- tables ----------------------------------------------------------------------- */
void wasm_rt_allocate_funcref_table(wasm_rt_funcref_table_t *table, uint32_t elements, uint32_t max_elements)
{
    table->data = (wasm_rt_funcref_t *)calloc(elements ? elements : 1, sizeof(wasm_rt_funcref_t));
    table->size = elements;
    table->max_size = max_elements;
}

void wasm_rt_free_funcref_table(wasm_rt_funcref_table_t *table)
{
    free(table->data);
    table->data = NULL;
}

uint32_t wasm_rt_grow_funcref_table(wasm_rt_funcref_table_t *table, uint32_t delta, wasm_rt_funcref_t init)
{
    return (uint32_t)-1;
}

void wasm_rt_allocate_externref_table(wasm_rt_externref_table_t *table, uint32_t elements, uint32_t max_elements)
{
    table->data = (wasm_rt_externref_t *)calloc(elements ? elements : 1, sizeof(wasm_rt_externref_t));
    table->size = elements;
    table->max_size = max_elements;
}

void wasm_rt_free_externref_table(wasm_rt_externref_table_t *table)
{
    free(table->data);
    table->data = NULL;
}

uint32_t wasm_rt_grow_externref_table(wasm_rt_externref_table_t *table, uint32_t delta, wasm_rt_externref_t init)
{
    return (uint32_t)-1;
}

/* ---- exceptions (setjmp/longjmp of the guest) ---------------------------------------
 * The game runs on one thread, so the state is plain statics. */
#define MAX_EXCEPTION_SIZE 256

static wasm_rt_tag_t sExceptionTag;
static uint8_t sException[MAX_EXCEPTION_SIZE];
static uint32_t sExceptionSize;
static wasm_rt_jmp_buf *sUnwindTarget;

void wasm_rt_load_exception(const wasm_rt_tag_t tag, uint32_t size, const void *values)
{
    if (size > MAX_EXCEPTION_SIZE)
    {
        wasm_rt_trap(WASM_RT_TRAP_EXHAUSTION);
    }
    sExceptionTag = tag;
    sExceptionSize = size;
    if (size)
    {
        memcpy(sException, values, size);
    }
}

WASM_RT_NO_RETURN void wasm_rt_throw(void)
{
    if (sUnwindTarget == NULL)
    {
        wasm_rt_trap(WASM_RT_TRAP_UNCAUGHT_EXCEPTION);
    }
    WASM_RT_LONGJMP(*sUnwindTarget, WASM_RT_TRAP_UNCAUGHT_EXCEPTION);
}

WASM_RT_UNWIND_TARGET *wasm_rt_get_unwind_target(void)
{
    return sUnwindTarget;
}

void wasm_rt_set_unwind_target(WASM_RT_UNWIND_TARGET *target)
{
    sUnwindTarget = target;
}

wasm_rt_tag_t wasm_rt_exception_tag(void)
{
    return sExceptionTag;
}

uint32_t wasm_rt_exception_size(void)
{
    return sExceptionSize;
}

void *wasm_rt_exception(void)
{
    return sException;
}
