#include <benchmark/benchmark.h>
#include <sys/single_threaded.h>
#include <memory>

struct Dummy {
  int x;
};

std::shared_ptr<Dummy> base = std::make_shared<Dummy>();

static void BM_SharedPtr_IncDec(benchmark::State& state) {
  for (auto _ : state) {
    std::shared_ptr<Dummy> local = base;
    benchmark::DoNotOptimize(local.use_count());
  }
}

BENCHMARK(BM_SharedPtr_IncDec);

static void BM_WeakPtr_IncDec(benchmark::State& state) {
  for (auto _ : state) {
    std::weak_ptr<Dummy> local = base;
    benchmark::DoNotOptimize(local.use_count());
  }
}

BENCHMARK(BM_WeakPtr_IncDec);

int main(int argc, char** argv) {
  // Force-set __libc_single_threaded to 0 to disable optimizations
  // for single-threaded apps.
  __libc_single_threaded = 0;
  ::benchmark::Initialize(&argc, argv);
  ::benchmark::RunSpecifiedBenchmarks();
}
