/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */
/*
 * gen_libsvm.c — synthetic libsvm (sparse) dataset generator for the liblinear
 * memory-capacity benchmark (run-liblinear-multi.sh / run_liblinear.sh).
 *
 * Usage:
 *   gen_libsvm <l> <k> <n> [seed] [mode] > out.svm
 *     l    : number of rows (instances)
 *     k    : nonzeros per row (k <= n)
 *     n    : feature dimension (max feature index)
 *     seed : RNG seed (default 1)
 *     mode : "identical" (default) or "varied"
 *
 * Output is libsvm text:  <label> <idx>:<val> <idx>:<val> ...
 * one row per line. Feature indices within a row are STRICTLY INCREASING and
 * <= n (required by liblinear's read_problem, which rejects non-increasing
 * idx).
 *
 * MODES
 * -----
 * identical (default): every row has the SAME k features (indices evenly spaced
 *   over [1..n], values 1..9 by column), only the +1/-1 label varies. The row
 * is built once and fwrite'd per row, so generation is disk-bound (~GB/s). Best
 *   for pure capacity loads and for primal/dual SVC (-s 1/2). NOTE: only k of
 * the n columns are ever nonzero and they're identical across rows, so column-
 *   oriented L1R solvers (-s 5/6) converge almost immediately on it.
 *
 * varied: each row draws k DISTINCT, strictly-increasing indices spread across
 *   the whole [1..n] space (one random pick per equal-width bucket), with
 * random values 1..9, and a label from a planted per-feature weight
 * (deterministic hash of the index) plus ~10% label noise. This makes (a) all n
 * columns active across the dataset and (b) columns differ across rows, so
 * L1-regular- ized coordinate-descent solvers (-s 6 L1R_LR, -s 5) have real
 * work across the feature space and iterate (longer run). Generation is
 * CPU-bound (per-row RNG
 *   + formatting) but uses a hand-rolled integer formatter to stay reasonably
 *   fast. File size is the same order as identical (~l*k*~10 bytes).
 *
 * Memory model this feeds: liblinear holds the matrix as feature_node =
 * {int index; double value;} = 16 bytes each, so the resident (row) footprint
 * is ~ 16 * l * (k+2) bytes. L1R solvers ALSO transpose the data (a 2nd
 * column-major copy of equal size), so for -s 5/6 resident ~ 2x that — size the
 * run with TARGET_GIB ~= half the desired resident footprint.
 *
 * Build:  gcc -O2 -Wall -o gen_libsvm gen_libsvm.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LABEL_NOISE_PCT \
  10 /* % of varied-mode labels flipped to break separability */

/* xorshift64 PRNG (deterministic given seed). */
static inline unsigned long long xs64(unsigned long long* s) {
  unsigned long long x = *s;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  *s = x;
  return x;
}

/* Deterministic per-feature "true weight" in ~[-0.5, 0.5] from the index, so
 * the varied-mode label carries a learnable signal without storing an n-vector.
 */
static inline double hashweight(unsigned long long idx) {
  unsigned long long h = idx * 0x9E3779B97F4A7C15ULL;
  h ^= h >> 32;
  return (double)((long long)(h & 0xFFFF) - 32768) / 65536.0;
}

/* Append an unsigned integer's decimal digits at p; return the new end pointer.
 */
static inline char* put_ull(char* p, unsigned long long v) {
  char tmp[20];
  int n = 0;
  do {
    tmp[n++] = (char)('0' + (int)(v % 10));
    v /= 10;
  } while (v);
  while (n)
    *p++ = tmp[--n];
  return p;
}

static int
gen_identical(long long l, long long k, long long n, unsigned long long seed);
static int
gen_varied(long long l, long long k, long long n, unsigned long long seed);

int main(int argc, char** argv) {
  if (argc < 4) {
    fprintf(
        stderr,
        "Usage: %s <l> <k> <n> [seed] [mode] > out.svm\n"
        "  l: rows  k: nonzeros/row (<=n)  n: feature dim\n"
        "  seed: RNG seed (default 1)  mode: identical (default) | varied\n",
        argv[0]);
    return 1;
  }

  long long l = atoll(argv[1]);
  long long k = atoll(argv[2]);
  long long n = atoll(argv[3]);
  unsigned long long seed = (argc > 4) ? strtoull(argv[4], NULL, 10) : 1ULL;
  const char* mode = (argc > 5) ? argv[5] : "identical";

  if (l <= 0 || k <= 0 || n <= 0) {
    fprintf(stderr, "ERROR: l, k, n must all be positive\n");
    return 1;
  }
  if (k > n) {
    fprintf(stderr, "ERROR: k (%lld) must be <= n (%lld)\n", k, n);
    return 1;
  }

  /* Large stdio buffer for throughput on a multi-hundred-GB write. */
  static char iobuf[1 << 22]; /* 4 MiB */
  setvbuf(stdout, iobuf, _IOFBF, sizeof(iobuf));

  int rc;
  if (strcmp(mode, "identical") == 0)
    rc = gen_identical(l, k, n, seed ? seed : 1ULL);
  else if (strcmp(mode, "varied") == 0)
    rc = gen_varied(l, k, n, seed ? seed : 1ULL);
  else {
    fprintf(stderr, "ERROR: unknown mode '%s' (use identical|varied)\n", mode);
    return 1;
  }
  if (rc != 0)
    return rc;

  if (fflush(stdout) != 0) {
    fprintf(stderr, "ERROR: write/flush failed\n");
    return 1;
  }
  return 0;
}

/* identical: build one row body, fwrite it per row, vary only the label. */
static int
gen_identical(long long l, long long k, long long n, unsigned long long seed) {
  long long stride = n / k;
  if (stride < 1)
    stride = 1;

  size_t body_cap = (size_t)k * 24 + 16;
  char* body = (char*)malloc(body_cap);
  if (!body) {
    fprintf(stderr, "ERROR: out of memory (body)\n");
    return 1;
  }
  size_t blen = 0;
  {
    long long idx = 1;
    for (long long j = 0; j < k; j++) {
      int val = (int)((j % 9) + 1); /* single-digit value 1..9 */
      int w = snprintf(body + blen, body_cap - blen, " %lld:%d", idx, val);
      if (w < 0 || (size_t)w >= body_cap - blen) {
        fprintf(stderr, "ERROR: row body buffer overflow\n");
        return 1;
      }
      blen += (size_t)w;
      idx += stride;
    }
  }

  size_t line_len = 2 + blen + 1; /* "<label>" + body + "\n" */
  char* linebuf = (char*)malloc(line_len);
  if (!linebuf) {
    fprintf(stderr, "ERROR: out of memory (line)\n");
    return 1;
  }
  memcpy(linebuf + 2, body, blen);
  linebuf[2 + blen] = '\n';

  unsigned long long rng = seed;
  for (long long i = 0; i < l; i++) {
    if (xs64(&rng) & 1ULL) {
      linebuf[0] = '+';
      linebuf[1] = '1';
    } else {
      linebuf[0] = '-';
      linebuf[1] = '1';
    }
    if (fwrite(linebuf, 1, line_len, stdout) != line_len) {
      fprintf(stderr, "ERROR: write failed\n");
      return 1;
    }
  }
  free(linebuf);
  free(body);
  return 0;
}

/* varied: per-row distinct indices spread across [1..n] (one per bucket),
 * random values, label from a planted hash weight + noise. Built into a reused
 * buffer with a hand-rolled integer formatter, then fwrite per row. */
static int
gen_varied(long long l, long long k, long long n, unsigned long long seed) {
  long long bucket = n / k;
  if (bucket < 1)
    bucket = 1;

  /* Worst-case line: label(2) + k * (" " + up-to-19-digit idx + ":" + 1-digit)
   * + "\n". */
  size_t cap = (size_t)k * 24 + 32;
  char* linebuf = (char*)malloc(cap);
  if (!linebuf) {
    fprintf(stderr, "ERROR: out of memory (line)\n");
    return 1;
  }

  unsigned long long rng = seed;
  for (long long i = 0; i < l; i++) {
    char* p = linebuf + 2; /* leave 2 bytes for the label, filled in last */
    double score = 0.0;
    for (long long j = 0; j < k; j++) {
      /* One pick inside bucket j -> strictly increasing, distinct, <= n. */
      unsigned long long r = xs64(&rng);
      long long idx =
          j * bucket + 1 + (long long)(r % (unsigned long long)bucket);
      if (idx > n)
        idx = n; /* safety clamp (shouldn't trigger) */
      int val = (int)(xs64(&rng) % 9) + 1; /* 1..9 */
      score += hashweight((unsigned long long)idx) * (double)val;
      *p++ = ' ';
      p = put_ull(p, (unsigned long long)idx);
      *p++ = ':';
      *p++ = (char)('0' + val);
    }
    /* Planted label + ~LABEL_NOISE_PCT% flips so it isn't trivially separable.
     */
    int label = (score > 0.0) ? 1 : -1;
    if ((int)(xs64(&rng) % 100) < LABEL_NOISE_PCT)
      label = -label;
    linebuf[0] = (label == 1) ? '+' : '-';
    linebuf[1] = '1';
    *p++ = '\n';

    size_t line_len = (size_t)(p - linebuf);
    if (fwrite(linebuf, 1, line_len, stdout) != line_len) {
      fprintf(stderr, "ERROR: write failed\n");
      return 1;
    }
  }
  free(linebuf);
  return 0;
}
