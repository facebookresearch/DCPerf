/*
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
*/

#include <arpa/inet.h>
#include <errno.h>
#include <gflags/gflags.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <iostream>

DEFINE_int64(workers, 1, "Number of threads to use. -1 to use all cores");
DEFINE_int64(duration_s, 1, "Test duration in seconds");
DEFINE_int64(nanosleep_ns, 100, "Duration of nanosleep in nanoseconds");
DEFINE_uint32(base_port, 16500, "Base port for TCP server");
DEFINE_string(
    syscalls,
    "",
    "Comma separated list of syscalls to test. All if empty");

DEFINE_bool(h, false, "help");
DECLARE_bool(help);
DECLARE_bool(helpshort);

#define TCP_HOST "127.0.0.1"
#define TCP_BUF_SZ 256

enum class Syscall {
  CLOCK_GETTIME,
  GETPID,
  NANOSLEEP,
  TCP,
};

std::unordered_map<std::string, Syscall> syscall_map = {
    {"clock_gettime", Syscall::CLOCK_GETTIME},
    {"getpid", Syscall::GETPID},
    {"nanosleep", Syscall::NANOSLEEP},
    {"tcp", Syscall::TCP},
};

std::unordered_map<Syscall, std::string> syscall_name_map = {
    {Syscall::CLOCK_GETTIME, "clock_gettime"},
    {Syscall::GETPID, "getpid"},
    {Syscall::NANOSLEEP, "nanosleep"},
    {Syscall::TCP, "tcp"},
};

bool run_worker;
unsigned long* worker_counters;
long nworkers;
std::unordered_set<Syscall> syscalls_to_test;
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

std::string list_available_syscalls() {
  std::string syscalls = "AVAILABLE SYSCALLS: \n";

  for (const auto& [syscall, _] : syscall_map) {
    syscalls += "\t" + syscall + "\n";
  }

  return syscalls;
}

void parse_syscall_list(const std::string& syscalls) {
  std::vector<std::string> syscall_list;
  std::string syscall;
  std::stringstream ss(syscalls);

  while (std::getline(ss, syscall, ',')) {
    syscall_list.push_back(syscall);
  }

  for (const auto& syscall : syscall_list) {
    if (syscall_map.find(syscall) == syscall_map.end()) {
      std::cout << "ERROR: Unknown syscall: " << syscall << std::endl;
      std::cout << list_available_syscalls() << std::endl;
      exit(EXIT_FAILURE);
    }
    syscalls_to_test.insert(syscall_map[syscall]);
  }

  if (syscalls_to_test.empty()) {
    for (const auto& [syscall, _] : syscall_map) {
      syscalls_to_test.insert(syscall_map[syscall]);
    }
  }
}

void* nanosleep_worker(void* arg) {
  int worker_id = static_cast<struct worker_arg*>(arg)->worker_id;
  unsigned long int count;
  for (count = 0; run_worker; count++) {
    nanosleep(&one_ns, nullptr);
  }
  worker_counters[worker_id] = count;
  return nullptr;
}

void* getpid_worker(void* arg) {
  int worker_id = static_cast<struct worker_arg*>(arg)->worker_id;
  unsigned long int count;
  volatile int pid;
  for (count = 0; run_worker; count++) {
    pid = getpid();
  }
  worker_counters[worker_id] = count;
  return nullptr;
}

void* clock_gettime_worker(void* arg) {
  int worker_id = static_cast<struct worker_arg*>(arg)->worker_id;
  unsigned long int count;
  struct timespec ts;
  volatile int res = 0;

  for (count = 0; run_worker; count++) {
    res = clock_gettime(CLOCK_REALTIME, &ts);
  }
  assert(res == 0);
  worker_counters[worker_id] = count;
  return nullptr;
}

void* launch_tcp_server(void* arg) {
  int server_fd, new_socket;
  const int port = static_cast<int>(reinterpret_cast<uintptr_t>(arg));

  struct sockaddr_in addr;
  int addrlen = sizeof(addr);
  char buf[TCP_BUF_SZ] = {0};
  char* msg = "";

  printf("Creating a new server on port %d\n", port);

  // Create a socket
  if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
    perror("Could not create socket");
    exit(EXIT_FAILURE);
  }

  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port);

  if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    perror("Could not bind");
    exit(EXIT_FAILURE);
  }

  if (listen(server_fd, 3) < 0) {
    perror("Could not listen");
    exit(EXIT_FAILURE);
  }

  while (true) {
    if ((new_socket = accept(
             server_fd, (struct sockaddr*)&addr, (socklen_t*)&addrlen)) < 0) {
      perror("Could not accept incoming connection");
      continue;
    }

    close(new_socket);
  }

  return nullptr;
}

void* tcp_worker(void* arg) {
  const int worker_id = static_cast<struct worker_arg*>(arg)->worker_id;
  const int port = FLAGS_base_port + worker_id;

  printf("TCP server on port %ld\n", port);

  pthread_t server_thread;
  int ret =
      pthread_create(&server_thread, nullptr, launch_tcp_server, (void*)port);
  if (ret != 0) {
    perror("Could not create server thread");
    exit(EXIT_FAILURE);
  }

  usleep(100 * 1000);

  sockaddr_in server_addr;
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(port);
  server_addr.sin_addr.s_addr = inet_addr(TCP_HOST);

  unsigned long int count;
  for (count = 0; run_worker; count++) {
    const int socket_desc = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_desc == -1) {
      perror("Could not create the socket");
      exit(EXIT_FAILURE);
    }

    const int res = connect(
        socket_desc, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (res < 0) {
      std::string err_msg = "Could not connect. res = " + std::to_string(res);
      perror(err_msg.c_str());
      exit(EXIT_FAILURE);
    }
    close(socket_desc);
  }

  worker_counters[worker_id] = count;

  return nullptr;
}

void benchmark_worker(typeof(void*(void*))* worker, const std::string& name) {
  long test_seconds = FLAGS_duration_s;

  worker_counters = new unsigned long[nworkers];
  pthread_t* workers = new pthread_t[nworkers];
  struct worker_arg* worker_args = new struct worker_arg[nworkers];
  // create threads
  run_worker = true;
  for (long i = 0; i < nworkers; ++i) {
    worker_args[i].worker_id = i;
    int ret = pthread_create(&workers[i], nullptr, worker, &worker_args[i]);
    if (ret != 0) {
      printf("Failed to create worker %ld - errno = %d\n", i, errno);
      exit(EXIT_FAILURE);
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
  printf("\t%lf seconds elapsed\n", time_elapsed);
  unsigned long total_nanosleep_calls = 0;
  for (long i = 0; i < nworkers; ++i) {
    printf("\t%s worker %ld: %lu\n", name.c_str(), i, worker_counters[i]);
    total_nanosleep_calls += worker_counters[i];
  }
  printf("\t%lu total %s calls\n", total_nanosleep_calls, name.c_str());
  printf(
      "%s %lf calls per second\n",
      name.c_str(),
      1.0 * total_nanosleep_calls / time_elapsed);
}

int main(int argc, char** argv) {
  gflags::SetUsageMessage("Syscall microbenchmarks.");

  // Workaround to use helpshort as the default help message
  gflags::ParseCommandLineNonHelpFlags(&argc, &argv, true);
  if (FLAGS_help || FLAGS_h) {
    FLAGS_help = false;
    FLAGS_helpshort = true;
  }
  gflags::HandleCommandLineHelpFlags();

  nworkers = FLAGS_workers;

  if (nworkers == -1) {
    nworkers = std::thread::hardware_concurrency();
  }

  one_ns.tv_nsec = FLAGS_nanosleep_ns;

  parse_syscall_list(FLAGS_syscalls);

  if (syscalls_to_test.find(Syscall::NANOSLEEP) != syscalls_to_test.end())
    benchmark_worker(nanosleep_worker, "nanosleep");

  if (syscalls_to_test.find(Syscall::GETPID) != syscalls_to_test.end())
    benchmark_worker(getpid_worker, "getpid");

  if (syscalls_to_test.find(Syscall::CLOCK_GETTIME) != syscalls_to_test.end())
    benchmark_worker(clock_gettime_worker, "clock_gettime");

  if (syscalls_to_test.find(Syscall::TCP) != syscalls_to_test.end())
    benchmark_worker(tcp_worker, "tcp");

  return 0;
}
