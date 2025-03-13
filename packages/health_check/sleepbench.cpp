/*
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
*/

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <cstdio>
#include <cstdlib>

bool run_worker;
unsigned long* worker_counters;
long nworkers;
struct timespec one_ns = {
    .tv_sec = 0,
    .tv_nsec = 100,
};

struct worker_arg {
  int worker_id;
};

void user_interrupt_handler(int signal) {
  if (signal == SIGINT) {
    run_worker = false;
  }
}

void* benchmark_worker(void* arg) {
  int worker_id = static_cast<struct worker_arg*>(arg)->worker_id;
  unsigned long int count;
  for (count = 0; run_worker; count++) {
    nanosleep(&one_ns, nullptr);
  }
  worker_counters[worker_id] = count;
  return nullptr;
}

int main(int argc, const char** argv) {
  if (argc < 2) {
    printf("Usage: %s <nworkers> [test-seconds]\n", argv[0]);
    exit(1);
  }
  nworkers = atol(argv[1]);
  long test_seconds = 0;
  if (argc >= 3) {
    test_seconds = atol(argv[2]);
  }
  if (argc >= 4) {
    long sleep_ns = atol(argv[3]);
    if (sleep_ns > 0) {
      one_ns.tv_nsec = sleep_ns;
    }
  }
  worker_counters = new unsigned long[nworkers];
  pthread_t* workers = new pthread_t[nworkers];
  struct worker_arg* worker_args = new struct worker_arg[nworkers];
  // create threads
  run_worker = true;
  for (long i = 0; i < nworkers; ++i) {
    worker_args[i].worker_id = i;
    int ret =
        pthread_create(&workers[i], nullptr, benchmark_worker, &worker_args[i]);
    if (ret != 0) {
      printf("Failed to create worker %ld - errno = %d\n", i, errno);
    }
  }
  // measure current time
  struct timespec start_time;
  clock_gettime(CLOCK_REALTIME, &start_time);
  // wait
  if (test_seconds > 0) {
    sleep(test_seconds);
    run_worker = false;
  } else {
    signal(SIGINT, user_interrupt_handler);
  }
  for (long i = 0; i < nworkers; ++i) {
    pthread_join(workers[i], nullptr);
  }
  // measure current time
  struct timespec end_time;
  clock_gettime(CLOCK_REALTIME, &end_time);
  double time_elapsed = (end_time.tv_sec - start_time.tv_sec) +
      (end_time.tv_nsec - start_time.tv_nsec) / 1e9;
  printf("%lf seconds elapsed\n", time_elapsed);
  unsigned long total_nanosleep_calls = 0;
  for (long i = 0; i < nworkers; ++i) {
    printf("worker %ld: %lu\n", i, worker_counters[i]);
    total_nanosleep_calls += worker_counters[i];
  }
  printf("%lu total nanosleep calls\n", total_nanosleep_calls);
  printf("%lf calls per second\n", 1.0 * total_nanosleep_calls / time_elapsed);
  return 0;
}
