// The benchmark's workloads in plain C: no badge.h, no libc beyond string.h,
// so the same file builds into the C app (WebAssembly, then native app.aot)
// and, for the "C in the firmware" column, into a test build of Solana OS.
// ../lua/main.lua does the same work in Lua; each workload's checksum must be
// the same in all of them (README.md).
#pragma once

#include <stddef.h>
#include <stdint.h>

// Mandelbrot, 320 x 240 over -2..1 x -1.2..1.2, at most 32 iterations a pixel.
// A unit is one row; it returns the row's iterations added up.
#define K_MANDEL_W 320
#define K_MANDEL_H 240
#define K_MANDEL_ITER 32
uint32_t k_mandel_row(int y);  // integers, 12 fractional bits
uint32_t k_mandelf_row(int y); // float (32-bit, as Lua's numbers)
uint32_t k_mandeld_row(int y); // double (C only: Lua has no 64-bit floats)

// Sieve of Eratosthenes up to K_SIEVE_N; flags has K_SIEVE_N + 1 bytes.
// Returns how many primes (564).
#define K_SIEVE_N 4096
uint32_t k_sieve(uint8_t *flags);

// SHA-256, one 64-byte block at a time (h: the state, m: 16 big-endian words).
// The checksum message is K_SHA_MSG bytes, byte i = (i * 7 + 3) & 255.
#define K_SHA_MSG 4096
void k_sha_init(uint32_t h[8]);
void k_sha_block(uint32_t h[8], const uint32_t m[16]);
void k_sha_msg_words(uint32_t *w); // the checksum message as K_SHA_MSG / 4 words
uint32_t k_sha_msg(void);          // its SHA-256 (with padding); the digest's first word

// A big buffer: K_BUF_WORDS 32-bit words (1 MB), worked on K_SEG_WORDS (64 KB)
// at a time. Fill writes a[i] = (i + base) ^ k in a loop; copy is memcpy.
#define K_BUF_WORDS (256 * 1024)
#define K_SEG_WORDS (16 * 1024)
void k_fill(uint32_t *a, uint32_t base, uint32_t n, uint32_t k);
void k_copy(uint32_t *dst, const uint32_t *src, uint32_t n);
uint32_t k_sum(const uint32_t *a, uint32_t n);

// A frame computed pixel by pixel (RGB565): row y of a w-pixel-wide frame at
// time t. Returns the row's pixel values added up.
uint32_t k_draw_row(uint16_t *row, int w, int y, int t);

// Particles bouncing in a 160 x 108 box (positions in 1/256 px), drawn 2 x 2.
#define K_PART_N 256
#define K_PART_W 160
#define K_PART_H 108
typedef struct {
    int32_t x, y, vx, vy;
} k_part;
void k_part_init(k_part *p);
void k_part_step(k_part *p, int i); // one particle, one step
uint32_t k_part_sum(const k_part *p);
void k_part_draw(uint16_t *frame, const k_part *p, int i);
extern const uint16_t k_part_colors[8]; // RGB565
#define K_PART_BG 0x0002              // RGB565 of 0x000010
