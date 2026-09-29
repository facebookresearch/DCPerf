/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

// A miniature of the TorchScript interpreter and c10 dispatcher that an
// inference server runs on the host around a model whose dense layers run on
// a GPU. It reproduces the shape of that work rather than its semantics: an
// interpreter loop over a boxed IValue stack, intrusive reference counting on
// tensors, storages, lists, and dicts, a per-operator dispatch table indexed
// by the highest dispatch key with an Autograd wrapper that redispatches to
// the CPU kernel, a heap allocation for every op output, and many distinct
// small op kernels, so the code footprint is broad and flat. The code has no
// AdSim or PyTorch dependencies so it can be tested standalone.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace facebook::cea::chips::adsim::torch_dispatch {

/* Base of reference-counted objects, like c10::intrusive_ptr_target. */
struct Target {
  std::atomic<int32_t> refcount{1};
  virtual ~Target() = default;
};

inline void incref(Target* t) {
  t->refcount.fetch_add(1, std::memory_order_relaxed);
}

inline void decref(Target* t) {
  if (1 == t->refcount.fetch_sub(1, std::memory_order_acq_rel)) {
    delete t;
  }
}

/* Owning pointer to a Target, like c10::intrusive_ptr. */
template <class T>
class Ptr {
 public:
  Ptr() = default;
  // Takes over the initial reference of a newly created object.
  static Ptr adopt(T* p) {
    Ptr r;
    r.p_ = p;
    return r;
  }
  Ptr(const Ptr& o) : p_(o.p_) {
    if (nullptr != p_) {
      incref(p_);
    }
  }
  Ptr(Ptr&& o) noexcept : p_(std::exchange(o.p_, nullptr)) {}
  // Copy-and-swap: the old object is released before the call returns.
  Ptr& operator=(const Ptr& o) {
    Ptr tmp(o);
    std::swap(p_, tmp.p_);
    return *this;
  }
  Ptr& operator=(Ptr&& o) noexcept {
    Ptr tmp(std::move(o));
    std::swap(p_, tmp.p_);
    return *this;
  }
  ~Ptr() {
    if (nullptr != p_) {
      decref(p_);
    }
  }
  T* get() const {
    return p_;
  }
  T* operator->() const {
    return p_;
  }
  T& operator*() const {
    return *p_;
  }
  T* release() {
    return std::exchange(p_, nullptr);
  }

 private:
  T* p_ = nullptr;
};

struct Storage final : Target {
  explicit Storage(size_t n)
      : nbytes(n), data(std::malloc(std::max<size_t>(n, 16))) {}
  Storage(const Storage&) = delete;
  Storage(Storage&&) = delete;
  Storage& operator=(const Storage&) = delete;
  Storage& operator=(Storage&&) = delete;
  ~Storage() override {
    std::free(data);
  }
  size_t nbytes;
  void* data;
};

enum class DType : uint8_t { kFloat, kLong, kHalf };

inline size_t itemSize(DType t) {
  return DType::kLong == t ? 8 : (DType::kHalf == t ? 2 : 4);
}

// Dispatch keys in priority order; the highest set bit selects the kernel.
enum DispatchKey : uint8_t {
  kCPU = 0,
  kAutogradCPU = 1,
  kNumKeys = 2,
};

struct TensorImpl final : Target {
  Ptr<Storage> storage;
  int64_t offset = 0; // In elements.
  int64_t rows = 1;
  int64_t cols = 1;
  DType dtype = DType::kFloat;
  uint8_t keys = (1 << kCPU) | (1 << kAutogradCPU);

  int64_t numel() const {
    return rows * cols;
  }
  char* bytes() const {
    return static_cast<char*>(storage->data) + offset * itemSize(dtype);
  }
  // 32-bit words readable from this tensor's data, whatever its dtype.
  int64_t wordSpan() const {
    int64_t avail = storage->nbytes - offset * itemSize(dtype);
    return std::max<int64_t>(
        0, std::min<int64_t>(numel() * itemSize(dtype), avail) / 4);
  }
};

using Tensor = Ptr<TensorImpl>;

/* Allocates a tensor with fresh storage, like at::empty_strided. */
inline Tensor emptyTensor(int64_t rows, int64_t cols, DType dtype) {
  auto* t = new TensorImpl();
  t->rows = std::max<int64_t>(1, rows);
  t->cols = std::max<int64_t>(1, cols);
  t->dtype = dtype;
  t->storage = Ptr<Storage>::adopt(new Storage(t->numel() * itemSize(dtype)));
  return Tensor::adopt(t);
}

/* A view sharing `base`'s storage, like at::as_strided. */
inline Tensor
viewTensor(const TensorImpl& base, int64_t offset, int64_t rows, int64_t cols) {
  auto* t = new TensorImpl();
  t->storage = base.storage;
  t->offset = base.offset + offset;
  t->rows = std::max<int64_t>(1, rows);
  t->cols = std::max<int64_t>(1, cols);
  t->dtype = base.dtype;
  t->keys = base.keys;
  return Tensor::adopt(t);
}

struct ListImpl;
struct DictImpl;

/* A tagged value holding a scalar or a reference-counted object, like
 * c10::IValue. */
class IValue {
 public:
  enum class Tag : uint8_t { kNone, kInt, kDouble, kTensor, kList, kDict };

  IValue() = default;
  static IValue fromInt(int64_t i) {
    IValue v;
    v.tag_ = Tag::kInt;
    v.u_.i = i;
    return v;
  }
  static IValue fromDouble(double d) {
    IValue v;
    v.tag_ = Tag::kDouble;
    v.u_.d = d;
    return v;
  }
  static IValue fromTensor(Tensor t) {
    return fromTarget(Tag::kTensor, t.release());
  }
  static IValue fromList(ListImpl* l);
  static IValue fromDict(DictImpl* d);

  IValue(const IValue& o) : u_(o.u_), tag_(o.tag_) {
    if (isPtr()) {
      incref(u_.p);
    }
  }
  IValue(IValue&& o) noexcept : u_(o.u_), tag_(o.tag_) {
    o.tag_ = Tag::kNone;
  }
  // Copy-and-swap: the old object is released before the call returns.
  IValue& operator=(const IValue& o) {
    IValue tmp(o);
    swap(tmp);
    return *this;
  }
  IValue& operator=(IValue&& o) noexcept {
    IValue tmp(std::move(o));
    swap(tmp);
    return *this;
  }
  ~IValue() {
    if (isPtr()) {
      decref(u_.p);
    }
  }

  bool isTensor() const {
    return Tag::kTensor == tag_;
  }
  bool isList() const {
    return Tag::kList == tag_;
  }
  bool isDict() const {
    return Tag::kDict == tag_;
  }
  TensorImpl* tensor() const {
    return static_cast<TensorImpl*>(u_.p);
  }
  ListImpl* list() const;
  DictImpl* dict() const;
  Tensor toTensor() const {
    incref(u_.p);
    return Tensor::adopt(tensor());
  }
  int64_t toInt() const {
    return Tag::kInt == tag_ ? u_.i
                             : (Tag::kDouble == tag_ ? int64_t(u_.d) : 0);
  }
  double toDouble() const {
    return Tag::kDouble == tag_ ? u_.d : double(toInt());
  }

 private:
  static IValue fromTarget(Tag tag, Target* p) {
    IValue v;
    v.tag_ = tag;
    v.u_.p = p;
    return v;
  }
  bool isPtr() const {
    return Tag::kTensor <= tag_;
  }
  void swap(IValue& o) noexcept {
    std::swap(u_, o.u_);
    std::swap(tag_, o.tag_);
  }

  union Payload {
    int64_t i;
    double d;
    Target* p;
  } u_{0};
  Tag tag_ = Tag::kNone;
};

using Stack = std::vector<IValue>;

struct ListImpl final : Target {
  std::vector<IValue> elems;
};

struct DictImpl final : Target {
  std::unordered_map<int64_t, IValue> items;
};

inline IValue IValue::fromList(ListImpl* l) {
  return fromTarget(Tag::kList, l);
}
inline IValue IValue::fromDict(DictImpl* d) {
  return fromTarget(Tag::kDict, d);
}
inline ListImpl* IValue::list() const {
  return static_cast<ListImpl*>(u_.p);
}
inline DictImpl* IValue::dict() const {
  return static_cast<DictImpl*>(u_.p);
}

/* One 64-byte object of the metadata heap that `chase_len` walks. */
struct alignas(64) ChaseNode {
  uint32_t next = 0;
  uint32_t zero = 0;
};

struct OpEntry;
using BoxedFn = void (*)(const OpEntry&, uint8_t keys, Stack&);

/* An operator's schema summary and dispatch table, like
 * c10::impl::OperatorEntry. */
struct OpEntry {
  int id = 0;
  int nargs = 0; // Stack slots the op consumes.
  int work_cap = 0; // Max elements an op touches per input.
  bool meta = false; // Runs the op's metadata prologue before its loop.
  bool shared_meta = false; // Also runs one of the shared metadata chains.
  int chase_len = 0; // Dependent metadata loads per call.
  uint32_t chase_nodes = 0;
  const ChaseNode* chase = nullptr; // Owned by the Code.
  std::array<BoxedFn, kNumKeys> table{};
};

inline thread_local uint32_t tls_chase_cursor = 0;

/* Walks `chase_len` nodes of the metadata heap, like the pointer loads from
 * an OperatorHandle to its OperatorEntry and KernelFunction, or from a
 * TensorImpl to its StorageImpl and DataPtr. Each call continues from where
 * the thread's last call stopped, so the nodes are rarely cached in L2, and
 * the op's loop bound depends on the last load.
 *
 * @return  Zero, loaded from the last node
 */
inline int64_t chaseMetadata(const OpEntry& op) {
  uint32_t n = tls_chase_cursor;
  if (op.chase_nodes <= n) {
    n %= op.chase_nodes;
  }
  for (int k = 0; op.chase_len > k; ++k) {
    n = op.chase[n].next;
  }
  tls_chase_cursor = n;
  return op.chase[n].zero;
}

// Profiling callbacks are checked on every call, like RecordFunction.
inline thread_local uint64_t tls_record_calls = 0;
inline std::atomic<bool> g_record_enabled{false};

/* Union of the dispatch keys of the top `nargs` stack entries, like
 * DispatchKeyExtractor::getDispatchKeySetBoxed. */
inline uint8_t keysFromStack(const Stack& s, int nargs) {
  uint8_t keys = 0;
  for (auto it = s.end() - nargs; s.end() != it; ++it) {
    if (it->isTensor()) {
      keys |= it->tensor()->keys;
    } else if (it->isList()) {
      for (const auto& e : it->list()->elems) {
        if (e.isTensor()) {
          keys |= e.tensor()->keys;
        }
      }
    }
  }
  return keys;
}

inline void callBoxed(const OpEntry& op, uint8_t keys, Stack& s) {
  if (g_record_enabled.load(std::memory_order_relaxed)) {
    ++tls_record_calls;
  }
  int key = 31 - __builtin_clz(uint32_t(keys) | 1u);
  op.table[key](op, keys, s);
}

/* Autograd kernel in inference: exclude the key and redispatch. */
inline void autogradKernel(const OpEntry& op, uint8_t keys, Stack& s) {
  callBoxed(op, keys & ~uint8_t(1 << kAutogradCPU), s);
}

// Op kernel families; the op id selects a family and its constants.
enum Family : int {
  kUnary = 0,
  kBinary = 1,
  kSlice = 2,
  kReduce = 3,
  kCat = 4,
  kIndex = 5,
  kTo = 6,
  kSplit = 7,
  kNumFamilies = 8,
};

constexpr int kMaxOps = 1024;

inline int familyOf(int id) {
  return id % kNumFamilies;
}

// Stack slots each family consumes: tensors, then one scalar where needed.
inline int nargsOf(int id) {
  switch (familyOf(id)) {
    case kBinary:
      return 3;
    case kCat:
    case kUnary:
    case kReduce:
      return 1;
    default:
      return 2;
  }
}

// Rows at which cat truncates, so tensors do not grow without bound.
constexpr int64_t kMaxRows = 4096;

/* Metadata prologue of an op, like the argument checks, output shape and
 * stride computation, and TensorIterator setup that an ATen op runs before
 * its loop. Each op ID gets its own straight-line chain of kMetaSteps steps
 * with ID-specific constants, so every call executes about 1.5 KB of code
 * that no other op shares. Each step ends in a check that never fails, which
 * keeps the branches predictable.
 *
 * Ops selected by `shared_meta_frac` also run the chain of op
 * `ID % kSharedMetaChains`. These few chains stand in for the dispatcher,
 * TensorIterator, and c10 code that every ATen op shares: about 120 KB that
 * misses in L1I but stays in L2.
 */
constexpr int kMetaSteps = 32;
constexpr int kSharedMetaChains = 64;

inline thread_local uint64_t tls_meta_sink = 0;

[[noreturn]] __attribute__((noinline, cold)) inline void metaCheckFailed(
    int id,
    int step) {
  std::fprintf(
      stderr, "torch_dispatch: op %d meta check %d failed\n", id, step);
  std::abort();
}

constexpr uint64_t metaMul(int id, int k) {
  return (((uint64_t(id) + 1) * 0x9E3779B97F4A7C15ull) ^
          ((uint64_t(k) + 1) * 0xC2B2AE3D27D4EB4Full)) |
      1;
}

constexpr int metaRot(int id, int k) {
  return (id * 7 + k * 13) % 63 + 1;
}

constexpr uint64_t metaAdd(int id, int k) {
  return (uint64_t(id) + 1) * (uint64_t(k) + 17) * 0x165667B19E3779F9ull;
}

template <int ID, int... Ks>
__attribute__((noinline)) uint64_t
opMetaChain(const int64_t* dims, std::integer_sequence<int, Ks...>) {
  uint64_t h = metaMul(ID, kMetaSteps);
  auto step = [&](uint64_t mul, int rot, uint64_t add, int64_t d, int k)
                  __attribute__((always_inline)) {
                    h = (h ^ uint64_t(d)) * mul;
                    h = ((h << rot) | (h >> (64 - rot))) + add;
                    if (__builtin_expect(add == h, 0)) {
                      metaCheckFailed(ID, k);
                    }
                  };
  (step(metaMul(ID, Ks), metaRot(ID, Ks), metaAdd(ID, Ks), dims[Ks % 4], Ks),
   ...);
  return h;
}

template <int ID>
uint64_t opMeta(const OpEntry& op, const Stack& s) {
  int64_t dims[4] = {op.id, op.work_cap, int64_t(s.size()), op.nargs};
  const int64_t n = std::min<int64_t>(op.nargs, int64_t(s.size()));
  for (int64_t i = 0; n > i; ++i) {
    const IValue& v = s[s.size() - 1 - i];
    if (v.isTensor()) {
      dims[i % 4] ^= v.tensor()->rows * 31 + v.tensor()->cols;
    }
  }
  return opMetaChain<ID>(dims, std::make_integer_sequence<int, kMetaSteps>{});
}

/* CPU kernel of op `ID`: unbox the arguments, compute the output metadata,
 * allocate, run a small loop with op-specific constants, and box the result.
 * Each ID is a distinct function, which spreads the code footprint.
 *
 * The loops use 32-bit integer arithmetic on the tensor words: storage is
 * uninitialized, like at::empty, and integer math keeps garbage and
 * reinterpreted dtypes from causing denormal or NaN assists. Float math is
 * modeled by the TensorOps kernel instead.
 */
template <int ID>
void cpuKernel(const OpEntry& op, uint8_t /*keys*/, Stack& s) {
  constexpr uint32_t kA = 2 * ID + 3;
  constexpr uint32_t kB = uint32_t(ID) * 2654435761u;
  constexpr int kFamily = ID % kNumFamilies;
  const int64_t cap = op.work_cap + (op.chase_len ? chaseMetadata(op) : 0);
  if (op.meta) {
    tls_meta_sink += opMeta<ID>(op, s);
  }
  if (op.shared_meta) {
    tls_meta_sink += opMeta<ID % kSharedMetaChains>(op, s);
  }
  auto words = [](const TensorImpl& t) {
    return reinterpret_cast<uint32_t*>(t.bytes());
  };

  if constexpr (kUnary == kFamily || kReduce == kFamily) {
    Tensor x = s.back().toTensor();
    s.pop_back();
    const int64_t cols = kReduce == kFamily ? 1 : x->cols;
    Tensor out = emptyTensor(x->rows, cols, DType::kFloat);
    const uint32_t* in = words(*x);
    uint32_t* o = words(*out);
    if constexpr (kUnary == kFamily) {
      const int64_t n = std::min({cap, x->wordSpan(), out->wordSpan()});
      for (int64_t i = 0; n > i; ++i) {
        o[i] = (in[i] * kA + kB) >> (ID % 3);
      }
    } else {
      const int64_t n = std::min(cap, x->wordSpan());
      uint32_t acc = kB;
      for (int64_t i = 0; n > i; ++i) {
        acc += in[i] * kA;
      }
      o[0] = acc;
    }
    s.push_back(IValue::fromTensor(std::move(out)));
  } else if constexpr (kBinary == kFamily) {
    const uint32_t alpha = uint32_t(s.back().toDouble() * 4) | 1;
    s.pop_back();
    Tensor y = s.back().toTensor();
    s.pop_back();
    Tensor x = s.back().toTensor();
    s.pop_back();
    Tensor out = emptyTensor(x->rows, x->cols, DType::kFloat);
    const uint32_t* a = words(*x);
    const uint32_t* b = words(*y);
    uint32_t* o = words(*out);
    const int64_t n = std::min({cap, x->wordSpan(), out->wordSpan()});
    const int64_t nb = std::max<int64_t>(1, y->wordSpan());
    for (int64_t i = 0; n > i; ++i) {
      o[i] = a[i] * kA + alpha * b[i % nb];
    }
    s.push_back(IValue::fromTensor(std::move(out)));
  } else if constexpr (kSlice == kFamily || kIndex == kFamily) {
    const int64_t arg = s.back().toInt();
    s.pop_back();
    Tensor x = s.back().toTensor();
    s.pop_back();
    const int64_t half = std::max<int64_t>(1, x->rows / 2);
    const int64_t start = std::abs(arg + ID) % (x->rows - half + 1);
    if constexpr (kSlice == kFamily) {
      s.push_back(
          IValue::fromTensor(viewTensor(*x, start * x->cols, half, x->cols)));
    } else {
      // index_select of `half` rows starting at `start`.
      const int64_t item = itemSize(x->dtype);
      Tensor out = emptyTensor(half, x->cols, x->dtype);
      const int64_t avail =
          int64_t(x->storage->nbytes) - (x->offset + start * x->cols) * item;
      const int64_t n = std::min({cap * 4, out->numel() * item, avail});
      const char* src = x->bytes() + start * x->cols * item;
      char* dst = out->bytes();
      for (int64_t i = 0; n > i; ++i) {
        dst[i] = src[i];
      }
      s.push_back(IValue::fromTensor(std::move(out)));
    }
  } else if constexpr (kCat == kFamily) {
    IValue list = std::move(s.back());
    s.pop_back();
    int64_t rows = 0;
    int64_t cols = 1;
    for (const auto& e : list.list()->elems) {
      rows += e.tensor()->rows;
      cols = std::max(cols, e.tensor()->cols);
    }
    Tensor out = emptyTensor(std::min(rows, kMaxRows), cols, DType::kFloat);
    uint32_t* o = words(*out);
    int64_t pos = 0;
    const int64_t total = out->wordSpan();
    for (const auto& e : list.list()->elems) {
      const TensorImpl* t = e.tensor();
      const uint32_t* in = words(*t);
      const int64_t n = std::min({cap, t->wordSpan(), total - pos});
      for (int64_t i = 0; n > i; ++i) {
        o[pos + i] = in[i];
      }
      pos += n;
    }
    s.push_back(IValue::fromTensor(std::move(out)));
  } else if constexpr (kTo == kFamily) {
    s.pop_back(); // Memory format argument; ignored.
    Tensor x = s.back().toTensor();
    s.pop_back();
    constexpr DType kOut =
        (ID / kNumFamilies) % 2 ? DType::kHalf : DType::kLong;
    Tensor out = emptyTensor(x->rows, x->cols, kOut);
    const uint32_t* in = words(*x);
    const int64_t n = std::min({cap, x->wordSpan(), out->numel()});
    if constexpr (DType::kHalf == kOut) {
      auto* o = reinterpret_cast<uint16_t*>(out->bytes());
      for (int64_t i = 0; n > i; ++i) {
        o[i] = uint16_t((in[i] * kA) >> 16);
      }
    } else {
      auto* o = reinterpret_cast<uint64_t*>(out->bytes());
      for (int64_t i = 0; n > i; ++i) {
        o[i] = uint64_t(in[i]) * kA;
      }
    }
    s.push_back(IValue::fromTensor(std::move(out)));
  } else {
    static_assert(kSplit == kFamily);
    const int64_t parts = 2 + std::abs(s.back().toInt()) % 3;
    s.pop_back();
    Tensor x = s.back().toTensor();
    s.pop_back();
    auto* list = new ListImpl();
    list->elems.reserve(parts);
    const int64_t rows = std::max<int64_t>(1, x->rows / parts);
    for (int64_t p = 0; parts > p; ++p) {
      const int64_t start = std::min(p * rows, x->rows - rows);
      list->elems.push_back(
          IValue::fromTensor(viewTensor(*x, start * x->cols, rows, x->cols)));
    }
    s.push_back(IValue::fromList(list));
  }
}

template <int... IDs>
constexpr std::array<BoxedFn, sizeof...(IDs)> makeCpuTable(
    std::integer_sequence<int, IDs...>) {
  return {&cpuKernel<IDs>...};
}

inline const std::array<BoxedFn, kMaxOps>& cpuKernels() {
  static constexpr auto kTable =
      makeCpuTable(std::make_integer_sequence<int, kMaxOps>{});
  return kTable;
}

enum class OpCode : uint8_t {
  kLoad,
  kMove,
  kStore,
  kLoadConst,
  kOp,
  kListConstruct,
  kListUnpack,
  kDictConstruct,
  kDictIndex,
  kDrop,
};

struct Instr {
  OpCode code;
  uint8_t n;
  uint16_t x;
};

/* A generated straight-line TorchScript-like graph. Registers 0..ninputs-1
 * receive the request's input tensors. */
struct Code {
  std::vector<Instr> instrs;
  std::vector<IValue> constants;
  std::vector<OpEntry> ops;
  std::shared_ptr<std::vector<ChaseNode>> chase; // Metadata heap of the ops.
  int nregs = 0;
  int ninputs = 0;
};

/* The interpreter loop, like InterpreterStateImpl::runTemplate. */
inline void run(const Code& code, std::vector<IValue>& regs, Stack& stack) {
  for (const Instr& in : code.instrs) {
    switch (in.code) {
      case OpCode::kLoad:
        stack.emplace_back(regs[in.x]);
        break;
      case OpCode::kMove:
        stack.emplace_back(std::move(regs[in.x]));
        break;
      case OpCode::kStore:
        regs[in.x] = std::move(stack.back());
        stack.pop_back();
        break;
      case OpCode::kLoadConst:
        stack.emplace_back(code.constants[in.x]);
        break;
      case OpCode::kOp: {
        const OpEntry& op = code.ops[in.x];
        callBoxed(op, keysFromStack(stack, op.nargs), stack);
        break;
      }
      case OpCode::kListConstruct: {
        auto* list = new ListImpl();
        list->elems.reserve(in.n);
        for (auto it = stack.end() - in.n; stack.end() != it; ++it) {
          list->elems.push_back(std::move(*it));
        }
        stack.resize(stack.size() - in.n);
        stack.push_back(IValue::fromList(list));
        break;
      }
      case OpCode::kListUnpack: {
        IValue list = std::move(stack.back());
        stack.pop_back();
        const auto& elems = list.list()->elems;
        for (int i = 0; in.n > i; ++i) {
          stack.push_back(elems[i % elems.size()]);
        }
        break;
      }
      case OpCode::kDictConstruct: {
        auto* dict = new DictImpl();
        for (int i = 0; in.n > i; ++i) {
          dict->items.emplace(
              in.x + i, std::move(stack[stack.size() - in.n + i]));
        }
        stack.resize(stack.size() - in.n);
        stack.push_back(IValue::fromDict(dict));
        break;
      }
      case OpCode::kDictIndex: {
        IValue dict = std::move(stack.back());
        stack.pop_back();
        stack.push_back(dict.dict()->items.at(in.x));
        break;
      }
      case OpCode::kDrop:
        stack.pop_back();
        break;
    }
  }
}

/* Parameters of a generated graph. */
struct GraphSpec {
  int nops = 256; // Distinct operators, at most kMaxOps.
  int ninstr_ops = 400; // Op calls in the graph.
  int nregs = 32;
  int ninputs = 8;
  double zipf_s = 0.8; // Skew of op popularity.
  int work_cap = 64;
  double meta_frac = 0; // Fraction of ops that run a metadata prologue.
  double shared_meta_frac = 0; // Fraction of ops that run a shared chain.
  int chase_len = 0; // Dependent metadata loads per op call.
  int chase_nodes = 0; // 64-byte nodes in the metadata heap.
  uint64_t seed = 1;
};

// Register indices are 16 bits wide in an Instr.
constexpr int kMaxRegs = 65536;

/* Rejects a spec whose graph cannot be generated: every op needs an input
 * tensor, and every register must be addressable by an Instr. */
inline void validateSpec(const GraphSpec& g) {
  if (1 > g.ninputs || kMaxRegs - 2 < g.ninputs) {
    throw std::invalid_argument(
        "TorchDispatch ninputs must be in [1, " + std::to_string(kMaxRegs - 2) +
        "]");
  }
  if (kMaxRegs < g.nregs) {
    throw std::invalid_argument(
        "TorchDispatch nregs must be at most " + std::to_string(kMaxRegs));
  }
}

/* Generates a graph whose op calls follow a Zipf distribution over `nops`
 * operators, with list construction around cat, list unpacking after split,
 * and occasional dict construction and lookup. */
inline Code buildCode(const GraphSpec& g) {
  validateSpec(g);
  Code code;
  code.nregs = std::max(g.nregs, g.ninputs + 2);
  code.ninputs = g.ninputs;
  std::mt19937_64 rng(g.seed);
  const int nops = std::clamp(g.nops, 1, kMaxOps);
  const bool chase = 0 < g.chase_len && 0 < g.chase_nodes;
  if (chase) {
    // One random cycle through all nodes (Sattolo), from its own generator so
    // the graph itself does not change.
    code.chase = std::make_shared<std::vector<ChaseNode>>(g.chase_nodes);
    auto& nodes = *code.chase;
    std::vector<uint32_t> perm(nodes.size());
    std::iota(perm.begin(), perm.end(), 0);
    std::mt19937_64 chase_rng(g.seed ^ 0x6368617365ull);
    for (size_t i = perm.size() - 1; 0 < i; --i) {
      std::swap(perm[i], perm[chase_rng() % i]);
    }
    for (size_t i = 0; nodes.size() > i; ++i) {
      nodes[i].next = perm[i];
    }
  }

  // Spread op ids over the whole kernel table, giving op i family
  // i % kNumFamilies so that small `nops` still covers every family. Ids stay
  // distinct: two ops of one family are at least kNumFamilies apart in i, so
  // their spread positions fall in different blocks of kNumFamilies ids.
  for (int i = 0; nops > i; ++i) {
    OpEntry e;
    const int spread = int(uint64_t(i) * kMaxOps / nops);
    e.id = spread - familyOf(spread) + i % kNumFamilies;
    e.nargs = nargsOf(e.id);
    e.work_cap = g.work_cap;
    // A fixed pseudo-random subset of op ids, so graphs agree on it.
    e.meta = std::fmod(e.id * 0.6180339887498949, 1.0) < g.meta_frac;
    e.shared_meta =
        std::fmod(e.id * 0.7548776662466927, 1.0) < g.shared_meta_frac;
    if (chase) {
      e.chase_len = g.chase_len;
      e.chase_nodes = uint32_t(g.chase_nodes);
      e.chase = code.chase->data();
    }
    e.table[kCPU] = cpuKernels()[e.id];
    e.table[kAutogradCPU] = &autogradKernel;
    code.ops.push_back(e);
  }
  std::vector<double> weights(nops);
  for (int i = 0; nops > i; ++i) {
    weights[i] = 1.0 / std::pow(i + 1.0, g.zipf_s);
  }
  std::shuffle(weights.begin(), weights.end(), rng);
  std::discrete_distribution<int> pick_op(weights.begin(), weights.end());

  for (int i = 0; 16 > i; ++i) {
    code.constants.push_back(IValue::fromInt(int64_t(rng() % 7)));
    code.constants.push_back(IValue::fromDouble(0.25 * (i + 1)));
  }
  // Registers that hold a tensor at this point of the graph.
  std::vector<int> live(g.ninputs);
  for (int i = 0; g.ninputs > i; ++i) {
    live[i] = i;
  }
  auto emit = [&](OpCode c, int x, int n = 0) {
    code.instrs.push_back(Instr{c, uint8_t(n), uint16_t(x)});
  };
  auto loadLive = [&]() {
    const int idx = static_cast<int>(rng() % live.size());
    int reg = live[idx];
    // Move out of a register a third of the time when others stay live.
    if (4 < live.size() && 0 == rng() % 3) {
      emit(OpCode::kMove, reg);
      live.erase(live.begin() + idx);
    } else {
      emit(OpCode::kLoad, reg);
    }
  };
  auto storeResult = [&]() {
    const int reg = static_cast<int>(rng() % code.nregs);
    emit(OpCode::kStore, reg);
    if (live.end() == std::find(live.begin(), live.end(), reg)) {
      live.push_back(reg);
    }
  };
  auto loadConst = [&](bool dbl) {
    emit(OpCode::kLoadConst, 2 * static_cast<int>(rng() % 16) + (dbl ? 1 : 0));
  };

  for (int k = 0; g.ninstr_ops > k; ++k) {
    const int op = pick_op(rng);
    switch (familyOf(code.ops[op].id)) {
      case kUnary:
      case kReduce:
        loadLive();
        break;
      case kBinary:
        loadLive();
        loadLive();
        loadConst(/*dbl=*/true);
        break;
      case kCat: {
        const int n = 2 + static_cast<int>(rng() % 4);
        for (int i = 0; n > i; ++i) {
          loadLive();
        }
        emit(OpCode::kListConstruct, 0, n);
        break;
      }
      default:
        loadLive();
        loadConst(/*dbl=*/false);
        break;
    }
    emit(OpCode::kOp, op);
    if (kSplit == familyOf(code.ops[op].id)) {
      emit(OpCode::kListUnpack, 0, 2);
      storeResult();
    }
    storeResult();

    // Every 16 ops, pack four tensors into a dict and read one back, like a
    // graph that groups its input tensors by key.
    if (15 == k % 16) {
      for (int i = 0; 4 > i; ++i) {
        loadLive();
      }
      const int base = int(rng() % 1000);
      emit(OpCode::kDictConstruct, base, 4);
      emit(OpCode::kDictIndex, base + int(rng() % 4));
      storeResult();
    }
  }
  return code;
}

/* Runs `code` once on fresh inputs of `rows` x `cols` floats.
 *
 * @return  The number of live registers at exit, so the work is not dead
 */
inline int64_t runOnce(
    const Code& code,
    int64_t rows,
    int64_t cols,
    std::vector<IValue>& regs,
    Stack& stack) {
  regs.assign(code.nregs, IValue());
  for (int i = 0; code.ninputs > i; ++i) {
    Tensor t = emptyTensor(rows, cols, DType::kFloat);
    auto* d = reinterpret_cast<uint32_t*>(t->bytes());
    for (int64_t j = 0; t->numel() > j; ++j) {
      d[j] = uint32_t(i * 131 + j);
    }
    regs[i] = IValue::fromTensor(std::move(t));
  }
  stack.clear();
  run(code, regs, stack);
  int64_t live = 0;
  for (const auto& r : regs) {
    live += r.isTensor();
  }
  regs.clear();
  stack.clear();
  return live;
}

} // namespace facebook::cea::chips::adsim::torch_dispatch
