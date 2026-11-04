# AdSim's Thrift libraries build with `thrift_add_library()`

**What this checks.** The CMake build of `if/` makes `fb303-cpp2`,
`AdSim-cpp2` and `Serialize-cpp2`, and `adsim_client` compiles and links
against them, when the sources go through the steps DCPerf's
`packages/adsim/build-adsim.sh` applies. CI cannot run it: it needs a Thrift
CMake package with `thrift_add_library()`, which the release DCPerf pins
(`build-deps.sh`) does not have.

**Not covered.** `adsim_server` and `adsim_server_torch` (they need FBGEMM,
libtorch and gengetopt), Treadmill, and running anything.

## Set up

- A prefix holding folly, fizz, wangle, mvfst, fb303 and Thrift built from
  the same revision, with Thrift's CMake package providing
  `thrift_add_library()`. It must be a single directory: `cmake.txt` passes it
  as `CMAKE_PREFIX_PATH`.
- A copy of `fbcode/cea/chips/adsim` as `<work>/adsim`, and of
  `benchpress/packages/adsim/{patches,buildfiles}` beside it.
- In `<work>/adsim/CMakeLists.txt`, drop what only the servers and the clang
  pin need: the `CMAKE_C_COMPILER`/`CMAKE_CXX_COMPILER` lines, the
  `GTest`, `LibIberty`, `Atomic`, `OpenMP` and `fbgemmLibrary` finds,
  `JEMALLOC_LIB`, `GENGETOPT_EXECUTABLE`, and build `cpp2/client` instead of
  `cpp2`.
- In `buildfiles/adsim/cmake.txt`, drop the `TORCH_ROOT` lookup and its two
  `-D` flags, and replace its final `make` with
  `make AdSim-cpp2 Serialize-cpp2 fb303-cpp2 adsim_client`.

## Run

In `<work>/adsim`: `patch -p1 --forward < ../patches/adsim.patch`, then in a
fresh `build/` run `cmake.txt` with `ADSIM_STAGING_DIR` set to the prefix.

## Assert

- `make` exits 0 and links `adsim_client`.
- `if/gen-cpp2/AdSim.h` includes its siblings as
  `"cea/chips/adsim/if/gen-cpp2/..."`.

`patch` stops at line 120 of `adsim.patch` with "malformed patch", after the
`AdSimService.h` hunk, exactly as in DCPerf, so the `if/CMakeLists.txt` and
`.thrift` hunks of that patch do not apply.

## Last run

2026-11-04, this revision, GCC 14.4, CMake 3.31, against folly,
fizz, wangle, mvfst, fb303 and Thrift from fbsource master that day: passed.
The parent revision, which still uses `CMake/ThriftLibrary.cmake`, also
passed against the same prefix.
