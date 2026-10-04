/*
 * Compiler support routines that the game code (built for the GNU target, for
 * GCC struct layout) expects from libgcc. Built here for the MSVC target, where
 * 64-bit division lowers to the CRT's own helpers.
 */
long long __divdi3(long long a, long long b) { return a / b; }
long long __moddi3(long long a, long long b) { return a % b; }
unsigned long long __udivdi3(unsigned long long a, unsigned long long b) { return a / b; }
unsigned long long __umoddi3(unsigned long long a, unsigned long long b) { return a % b; }
