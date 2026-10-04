/*
 * Included by tools/wasm_to_c.py into the wasm2c -impl.h after its arithmetic
 * helpers: GBA behaviour instead of wasm traps.
 */

/* Division by zero does not trap on the GBA: the game's libgcc __divsi3/__modsi3 call
 * __div0 and return 0. INT_MIN / -1 wraps to INT_MIN (remainder 0). */
#undef DIV_S
#define DIV_S(ut, min, x, y)                                     \
  ((UNLIKELY((y) == 0))                  ? (ut)0                 \
   : (UNLIKELY((x) == min && (y) == -1)) ? (ut)(x)               \
                                         : (ut)((x) / (y)))
#undef REM_S
#define REM_S(ut, min, x, y)                                 \
  ((UNLIKELY((y) == 0))                  ? (ut)0             \
   : (UNLIKELY((x) == min && (y) == -1)) ? 0                 \
                                         : (ut)((x) % (y)))
#undef DIV_U
#define DIV_U(x, y) ((UNLIKELY((y) == 0)) ? 0 : ((x) / (y)))
#undef REM_U
#define REM_U(x, y) ((UNLIKELY((y) == 0)) ? 0 : ((x) % (y)))

/* Indirect calls: decomps call through function pointers whose type differs from the
 * function's (K&R pointers, tables of mixed handlers such as m4a's jump table). The ARM
 * calling convention tolerates that and so do the host ABIs (arguments in registers,
 * the callee ignores extras), so only check that the slot holds a function. */
void agbw_bad_indirect_call(uint32_t index);
#undef CHECK_CALL_INDIRECT
#define CHECK_CALL_INDIRECT(table, ft, x) \
  (LIKELY((x) < table.size && table.data[x].func) || (agbw_bad_indirect_call(x), 0))
