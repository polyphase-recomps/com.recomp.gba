/*
 * Included by tools/wasm_to_c.py into the wasm2c -impl.h, just before the load and
 * store helpers are instantiated: memory accesses follow agbw.h (masked or clamped
 * addresses, little-endian buffer, no bounds checks, optionally paged read-only data).
 */
#include "agbw.h"

#if AGBW_PAGED
/* agbw_guest.c */
extern uint8_t *agbw_ram;                        /* [AGBW_RAM_BASE, AGBW_MEM_BYTES) */
extern uint8_t *agbw_rom_pages[];                /* resident page or NULL */
uint8_t *agbw_rom_fault(u32 addr);               /* loads the page of addr, returns its base */
uint8_t *agbw_rom_cross(u32 addr, u32 n, int store); /* an access spanning two pages */
void agbw_rom_pin(u32 addr);                     /* the page of addr was written: keep it */

static inline uint8_t *agbw_addr(u32 a, u32 n, int store)
{
    uint8_t *page;

    a = AGBW_OFFSET(a);
    if (LIKELY(a >= AGBW_RAM_BASE)) return agbw_ram + (a - AGBW_RAM_BASE);
    if (UNLIKELY((a & (AGBW_PAGE_BYTES - 1u)) + n > AGBW_PAGE_BYTES)) return agbw_rom_cross(a, n, store);
    page = agbw_rom_pages[a >> AGBW_PAGE_SHIFT];
    if (UNLIKELY(page == NULL)) page = agbw_rom_fault(a);
    if (UNLIKELY(store)) agbw_rom_pin(a);
    return page + (a & (AGBW_PAGE_BYTES - 1u));
}

#undef MEM_ADDR
#define MEM_ADDR(mem, addr, n) agbw_addr((u32)(addr), (u32)(n), 0)
#define AGBW_STORE_ADDR(mem, addr, n) agbw_addr((u32)(addr), (u32)(n), 1)
#else
#undef MEM_ADDR
#define MEM_ADDR(mem, addr, n) (&(mem)->data[AGBW_OFFSET(addr)])
#define AGBW_STORE_ADDR(mem, addr, n) MEM_ADDR(mem, addr, n)
#endif
#undef MEM_ADDR_MEMOP
#define MEM_ADDR_MEMOP(mem, addr, n) MEM_ADDR(mem, addr, n)
#undef RANGE_CHECK
#define RANGE_CHECK(mem, offset, len)
#undef MEMCHECK_DEFAULT32
#define MEMCHECK_DEFAULT32(mem, local_memory_size, a, t)
#undef MEMCHECK_GENERAL
#define MEMCHECK_GENERAL(mem, a, t)
#undef LOAD_DATA
#define LOAD_DATA(m, o, i, s) wasm_rt_memcpy(MEM_ADDR(&(m), o, s), i, s)

/* memory.fill / memory.copy (memset, memcpy and DMA copies of the game): tools/wasm_to_c.py
 * renames wasm2c's own versions so these are used; paged, they go a page at a time */
#if AGBW_PAGED
static inline u32 agbw_page_left(u32 a)
{
    a = AGBW_OFFSET(a);
    return a >= AGBW_RAM_BASE ? 0xFFFFFFFFu : AGBW_PAGE_BYTES - (a & (AGBW_PAGE_BYTES - 1u));
}

static inline void memory_fill(wasm_rt_memory_t *mem, u64 d, u32 val, u64 n)
{
    while (n)
    {
        u32 chunk = agbw_page_left((u32)d);

        if (chunk > n) chunk = (u32)n;
        memset(agbw_addr((u32)d, chunk, 1), (int)val, chunk);
        d += chunk;
        n -= chunk;
    }
}

static inline void memory_copy(wasm_rt_memory_t *dest, const wasm_rt_memory_t *src, u64 dest_addr, u64 src_addr, u64 n)
{
    u32 d = (u32)dest_addr, s = (u32)src_addr;

    if (AGBW_OFFSET(d) >= AGBW_RAM_BASE && AGBW_OFFSET(s) >= AGBW_RAM_BASE && AGBW_OFFSET(d) + n <= AGBW_ALLOC_BYTES &&
        AGBW_OFFSET(s) + n <= AGBW_ALLOC_BYTES)
    {
        memmove(agbw_addr(d, 1, 1), agbw_addr(s, 1, 0), (size_t)n);
        return;
    }
    if (d > s && d < s + n)
    {
        /* overlapping, destination above: backwards a byte at a time (never seen in
         * practice for read-only data) */
        while (n--) *agbw_addr(d + (u32)n, 1, 1) = *agbw_addr(s + (u32)n, 1, 0);
        return;
    }
    while (n)
    {
        u32 chunk = agbw_page_left(d), left = agbw_page_left(s);
        uint8_t *to;

        if (chunk > left) chunk = left;
        if (chunk > n) chunk = (u32)n;
        to = agbw_addr(d, chunk, 1); /* first: a fault for the source cannot evict it */
        memmove(to, agbw_addr(s, chunk, 0), chunk);
        d += chunk;
        s += chunk;
        n -= chunk;
    }
}
#else
static inline void memory_fill(wasm_rt_memory_t *mem, u64 d, u32 val, u64 n)
{
    memset(MEM_ADDR(mem, d, n), (int)val, (size_t)n);
}

static inline void memory_copy(wasm_rt_memory_t *dest, const wasm_rt_memory_t *src, u64 dest_addr, u64 src_addr, u64 n)
{
    memmove(MEM_ADDR(dest, dest_addr, n), MEM_ADDR(src, src_addr, n), (size_t)n);
}
#endif

/* Debug builds (-DAGBW_WATCH): every guest store is reported to agbw_watch_store, which
 * logs the ones that hit the range in the AGBW_WATCH environment variable ("lo-hi", hex)
 * with a host backtrace (wasm2c functions carry the game's function names). */
#ifdef AGBW_WATCH
void agbw_watch_store(u64 addr, u32 size, u64 value);
#define AGBW_STORE_HOOK(addr, size, value) agbw_watch_store(addr, size, (u64)(value))
#else
#define AGBW_STORE_HOOK(addr, size, value)
#endif

#if AGBW_BIG_ENDIAN || defined(AGBW_WATCH) || AGBW_PAGED
#if AGBW_BIG_ENDIAN
static inline f32 agbw_swap_f32(f32 v)
{
    u32 u;
    wasm_rt_memcpy(&u, &v, 4);
    u = __builtin_bswap32(u);
    wasm_rt_memcpy(&v, &u, 4);
    return v;
}

static inline f64 agbw_swap_f64(f64 v)
{
    u64 u;
    wasm_rt_memcpy(&u, &v, 8);
    u = __builtin_bswap64(u);
    wasm_rt_memcpy(&v, &u, 8);
    return v;
}

/* little-endian <-> host for every type wasm2c loads and stores */
#define AGBW_LE(v)                                    \
    _Generic((v),                                     \
        u8: (v), s8: (v),                             \
        u16: (u16)__builtin_bswap16((u16)(v)),        \
        s16: (s16)__builtin_bswap16((u16)(v)),        \
        u32: (u32)__builtin_bswap32((u32)(v)),        \
        s32: (s32)__builtin_bswap32((u32)(v)),        \
        u64: (u64)__builtin_bswap64((u64)(v)),        \
        s64: (s64)__builtin_bswap64((u64)(v)),        \
        f32: agbw_swap_f32((f32)(v)),                 \
        f64: agbw_swap_f64((f64)(v)))
#else
#define AGBW_LE(v) (v)
#endif

#undef DEFINE_LOAD
#define DEFINE_LOAD(name, t1, t2, t3, force_read)                             \
  static inline t3 name##_unchecked(uint8_t* const wasm_rt_local_memory_base, \
                                    wasm_rt_memory_t* mem, u64 addr) {        \
    t1 result;                                                                \
    wasm_rt_memcpy(&result, MEM_ADDR_MEMOP(mem, addr, sizeof(t1)),            \
                   sizeof(t1));                                               \
    result = AGBW_LE(result);                                                 \
    return (t3)(t2)result;                                                    \
  }                                                                           \
  DEF_MEM_CHECKS0(name, _, t1, return, t3)

#undef DEFINE_STORE
#define DEFINE_STORE(name, t1, t2)                                     \
  static inline void name##_unchecked(                                 \
      uint8_t* const wasm_rt_local_memory_base, wasm_rt_memory_t* mem, \
      u64 addr, t2 value) {                                            \
    t1 wrapped = (t1)value;                                            \
    AGBW_STORE_HOOK(addr, sizeof(t1), wrapped);                        \
    wrapped = AGBW_LE(wrapped);                                        \
    wasm_rt_memcpy(AGBW_STORE_ADDR(mem, addr, sizeof(t1)), &wrapped,   \
                   sizeof(t1));                                        \
  }                                                                    \
  DEF_MEM_CHECKS1(name, _, t1, , void, t2)
#endif
