/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * LD_PRELOAD library to distribute outgoing TCP connections across multiple
 * source IPv6 addresses. This allows a single client process to create
 * far more connections than the ~64K ephemeral port limit per source IP.
 *
 * Environment variables:
 *   BIND_ADDRESSES  Comma-separated list of IPv6 addresses to round-robin.
 *                   e.g. "2401:db00::1,2401:db00::2,2401:db00::3"
 *   BIND_ADDRESS    Single IPv6 address (legacy, used if BIND_ADDRESSES unset)
 *
 * Usage:
 *   gcc -shared -fPIC -o bind_source.so bind_source.c -ldl
 *
 *   # Single IP (legacy):
 *   BIND_ADDRESS="2401:db00::1" LD_PRELOAD=./bind_source.so ./ucachebench_client
 *
 *   # Multiple IPs (round-robin):
 *   BIND_ADDRESSES="2401:db00::1,2401:db00::2,2401:db00::3" \
 *     LD_PRELOAD=./bind_source.so ./ucachebench_client --additional_fanout=60000
 *
 * With N source IPs and widened port range (1024-65535), each IP supports
 * ~64K connections. 16 IPs = ~1M connections from a single process.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/socket.h>
#include <errno.h>
#include <stdatomic.h>

#define MAX_BIND_ADDRS 64

static struct sockaddr_in6 g_bind_addrs[MAX_BIND_ADDRS];
static int g_num_addrs = 0;
static int g_initialized = 0;
static atomic_uint g_next_addr = 0;
static int (*real_connect)(int, const struct sockaddr *, socklen_t) = NULL;

static int parse_addr(const char *str, struct sockaddr_in6 *out) {
  memset(out, 0, sizeof(*out));
  out->sin6_family = AF_INET6;
  out->sin6_port = 0;
  return inet_pton(AF_INET6, str, &out->sin6_addr) == 1 ? 0 : -1;
}

static void init_once(void) {
  if (g_initialized) return;
  g_initialized = 1;

  real_connect = dlsym(RTLD_NEXT, "connect");
  if (!real_connect) {
    fprintf(stderr, "bind_source: failed to find real connect()\n");
    return;
  }

  /* Try BIND_ADDRESSES first (comma-separated list) */
  const char *addrs = getenv("BIND_ADDRESSES");
  if (addrs && addrs[0] != '\0') {
    char buf[4096];
    strncpy(buf, addrs, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *saveptr = NULL;
    char *tok = strtok_r(buf, ",", &saveptr);
    while (tok && g_num_addrs < MAX_BIND_ADDRS) {
      /* Skip leading whitespace */
      while (*tok == ' ') tok++;
      if (*tok == '\0') { tok = strtok_r(NULL, ",", &saveptr); continue; }

      if (parse_addr(tok, &g_bind_addrs[g_num_addrs]) == 0) {
        g_num_addrs++;
      } else {
        fprintf(stderr, "bind_source: invalid address '%s', skipping\n", tok);
      }
      tok = strtok_r(NULL, ",", &saveptr);
    }

    if (g_num_addrs > 0) {
      fprintf(stderr, "bind_source: round-robin across %d source IPs\n",
              g_num_addrs);
    }
    return;
  }

  /* Fallback to single BIND_ADDRESS */
  const char *addr = getenv("BIND_ADDRESS");
  if (addr && addr[0] != '\0') {
    if (parse_addr(addr, &g_bind_addrs[0]) == 0) {
      g_num_addrs = 1;
      fprintf(stderr, "bind_source: will bind to %s\n", addr);
    } else {
      fprintf(stderr, "bind_source: invalid BIND_ADDRESS '%s'\n", addr);
    }
  }
}

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
  init_once();

  if (g_num_addrs > 0 && addr->sa_family == AF_INET6) {
    /* Check if socket is already bound */
    struct sockaddr_in6 current;
    socklen_t len = sizeof(current);
    if (getsockname(sockfd, (struct sockaddr *)&current, &len) == 0) {
      /* Only bind if not already bound (port == 0 means unbound) */
      if (current.sin6_port == 0 &&
          memcmp(&current.sin6_addr, &in6addr_any, sizeof(in6addr_any)) == 0) {
        /* Round-robin across source addresses */
        unsigned int idx = atomic_fetch_add(&g_next_addr, 1) % g_num_addrs;
        bind(sockfd, (struct sockaddr *)&g_bind_addrs[idx],
             sizeof(g_bind_addrs[idx]));
      }
    }
  }

  return real_connect(sockfd, addr, addrlen);
}
