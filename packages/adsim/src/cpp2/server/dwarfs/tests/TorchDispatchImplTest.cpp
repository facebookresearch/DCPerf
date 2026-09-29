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

#include <cea/chips/adsim/cpp2/server/dwarfs/TorchDispatchImpl.h>

#include <cstdint>
#include <numeric>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace facebook::cea::chips::adsim::torch_dispatch {
namespace {

constexpr uint8_t kCpuOnly = 1 << kCPU;
constexpr uint8_t kCpuAndAutograd = (1 << kCPU) | (1 << kAutogradCPU);

// Counts its destructions so tests can check that references are released.
struct Counted final : Target {
  explicit Counted(int* destroyed) : destroyed_(destroyed) {}
  ~Counted() override {
    ++*destroyed_;
  }
  int* destroyed_;
};

// A float tensor whose 32-bit words are 1, 2, 3, ...
Tensor iotaTensor(int64_t rows, int64_t cols) {
  Tensor t = emptyTensor(rows, cols, DType::kFloat);
  auto* w = reinterpret_cast<uint32_t*>(t->bytes());
  for (int64_t i = 0; t->numel() > i; ++i) {
    w[i] = static_cast<uint32_t>(i + 1);
  }
  return t;
}

// The first `n` elements of type T in the tensor held by `v`.
template <class T = uint32_t>
std::vector<T> elems(const IValue& v, int64_t n) {
  const auto* p = reinterpret_cast<const T*>(v.tensor()->bytes());
  return std::vector<T>(p, p + n);
}

// {first, first + 1, ..., first + n - 1}.
std::vector<uint32_t> iota(uint32_t first, int n) {
  std::vector<uint32_t> out(n);
  std::iota(out.begin(), out.end(), first);
  return out;
}

// Runs the CPU kernel of op `ID` on `s` with a cap that covers whole tensors.
template <int ID>
void runCpuOp(Stack& s) {
  OpEntry op;
  op.id = ID;
  op.nargs = nargsOf(ID);
  op.work_cap = 1 << 20;
  cpuKernel<ID>(op, kCpuOnly, s);
}

// The shape of the graphs in the measured AdSim configuration.
GraphSpec measuredSpec() {
  GraphSpec g;
  g.nops = 1024;
  g.ninstr_ops = 1000;
  g.nregs = 32;
  g.ninputs = 8;
  g.zipf_s = 0.85;
  g.work_cap = 16;
  g.meta_frac = 0.2;
  g.shared_meta_frac = 1.0;
  g.chase_len = 2;
  g.chase_nodes = 65536;
  return g;
}

TEST(TorchDispatchImplTest, PtrReleasesOnLastReference) {
  int destroyed = 0;
  {
    auto a = Ptr<Counted>::adopt(new Counted(&destroyed));
    Ptr<Counted> b = a;
    EXPECT_EQ(2, a->refcount.load());
    Ptr<Counted> c = std::move(b);
    EXPECT_EQ(2, a->refcount.load());
    a = Ptr<Counted>();
    EXPECT_EQ(1, c->refcount.load());
    EXPECT_EQ(0, destroyed);
  }
  EXPECT_EQ(1, destroyed);
}

TEST(TorchDispatchImplTest, IValueCountsTensorReferences) {
  Tensor t = emptyTensor(2, 3, DType::kFloat);
  TensorImpl* impl = t.get();
  IValue v = IValue::fromTensor(std::move(t));
  EXPECT_TRUE(v.isTensor());
  EXPECT_EQ(1, impl->refcount.load());
  EXPECT_EQ(6, impl->numel());
  IValue copy = v;
  EXPECT_EQ(2, impl->refcount.load());
  Tensor u = copy.toTensor();
  EXPECT_EQ(3, impl->refcount.load());
  copy = IValue();
  EXPECT_EQ(2, impl->refcount.load());
}

TEST(TorchDispatchImplTest, IValueScalarConversions) {
  EXPECT_EQ(7, IValue::fromInt(7).toInt());
  EXPECT_EQ(7.0, IValue::fromInt(7).toDouble());
  EXPECT_EQ(2, IValue::fromDouble(2.75).toInt());
  EXPECT_EQ(0.5, IValue::fromDouble(0.5).toDouble());
  EXPECT_EQ(0, IValue().toInt());
  EXPECT_FALSE(IValue::fromInt(1).isTensor());
}

TEST(TorchDispatchImplTest, ViewSharesStorageAndClampsSpan) {
  Tensor base = emptyTensor(4, 8, DType::kFloat);
  Tensor view = viewTensor(*base, 2 * 8, 2, 8);
  EXPECT_EQ(base->storage.get(), view->storage.get());
  EXPECT_EQ(2, base->storage->refcount.load());
  EXPECT_EQ(base->bytes() + 2 * 8 * sizeof(float), view->bytes());
  EXPECT_EQ(16, view->wordSpan());
  // A view that runs past its storage spans only the bytes that remain.
  Tensor tail = viewTensor(*base, 3 * 8, 4, 8);
  EXPECT_EQ(8, tail->wordSpan());
  EXPECT_EQ(3, emptyTensor(2, 3, DType::kHalf)->wordSpan());
}

void recordKeys(const OpEntry& /*op*/, uint8_t keys, Stack& s) {
  s.push_back(IValue::fromInt(keys));
}

TEST(TorchDispatchImplTest, AutogradRedispatchesToCpu) {
  OpEntry op;
  op.table[kCPU] = &recordKeys;
  op.table[kAutogradCPU] = &autogradKernel;
  Stack s;
  callBoxed(op, kCpuAndAutograd, s);
  // The CPU kernel ran once, with the Autograd key excluded.
  ASSERT_EQ(1, s.size());
  EXPECT_EQ(kCpuOnly, s.back().toInt());
  callBoxed(op, kCpuOnly, s);
  EXPECT_EQ(kCpuOnly, s.back().toInt());
  // An empty key set selects the lowest key.
  callBoxed(op, 0, s);
  EXPECT_EQ(0, s.back().toInt());
}

TEST(TorchDispatchImplTest, KeysFromStackUnionsTopTensorsAndLists) {
  Tensor a = emptyTensor(1, 1, DType::kFloat);
  a->keys = 1 << kCPU;
  Tensor b = emptyTensor(1, 1, DType::kFloat);
  b->keys = 1 << kAutogradCPU;
  auto* list = new ListImpl();
  list->elems.push_back(IValue::fromTensor(std::move(b)));
  Stack s;
  s.push_back(IValue::fromTensor(std::move(a)));
  s.push_back(IValue::fromList(list));
  s.push_back(IValue::fromInt(3));
  EXPECT_EQ(kCpuAndAutograd, keysFromStack(s, 3));
  EXPECT_EQ(1 << kAutogradCPU, keysFromStack(s, 2));
  EXPECT_EQ(0, keysFromStack(s, 1));
}

TEST(TorchDispatchImplTest, ElementwiseFamilies) {
  // Op constants: kA = 2 * ID + 3 and kB = ID * 2654435761.
  std::vector<uint32_t> unary;
  std::vector<uint32_t> binary;
  uint32_t reduce = 3u * 2654435761u;
  for (uint32_t w = 1; 32 >= w; ++w) {
    unary.push_back(w * 3u);
    binary.push_back(w * 5u + ((w - 1) % 8 + 1));
    reduce += w * 9u;
  }

  // Unary op 0 keeps the shape.
  Stack s;
  s.push_back(IValue::fromTensor(iotaTensor(4, 8)));
  runCpuOp<0>(s);
  ASSERT_EQ(1, s.size());
  EXPECT_EQ(4, s.back().tensor()->rows);
  EXPECT_EQ(8, s.back().tensor()->cols);
  EXPECT_EQ(unary, elems(s.back(), 32));

  // Binary op 1 broadcasts y; the alpha argument 0.25 becomes 1.
  s.clear();
  s.push_back(IValue::fromTensor(iotaTensor(4, 8)));
  s.push_back(IValue::fromTensor(iotaTensor(1, 8)));
  s.push_back(IValue::fromDouble(0.25));
  runCpuOp<1>(s);
  ASSERT_EQ(1, s.size());
  EXPECT_EQ(binary, elems(s.back(), 32));

  // Reduce op 3 has one column, and word 0 folds every input word.
  s.clear();
  s.push_back(IValue::fromTensor(iotaTensor(4, 8)));
  runCpuOp<3>(s);
  ASSERT_EQ(1, s.size());
  EXPECT_EQ(4, s.back().tensor()->rows);
  EXPECT_EQ(1, s.back().tensor()->cols);
  EXPECT_EQ(reduce, elems(s.back(), 1)[0]);
}

TEST(TorchDispatchImplTest, ToFamilyConvertsDtype) {
  // Op 6 converts to int64 and op 14 to fp16, scaling by kA = 2 * ID + 3.
  // Large words make the fp16 results, the high halves, nonzero.
  std::vector<uint64_t> longs;
  std::vector<uint16_t> halves;
  for (uint32_t w = 1; 8 >= w; ++w) {
    longs.push_back(uint64_t(w) * 15u);
    halves.push_back(static_cast<uint16_t>(((w << 20) * 31u) >> 16));
  }
  auto largeTensor = []() {
    Tensor t = iotaTensor(2, 4);
    auto* w = reinterpret_cast<uint32_t*>(t->bytes());
    for (int64_t i = 0; t->numel() > i; ++i) {
      w[i] <<= 20;
    }
    return t;
  };

  // The second argument, the memory format, is ignored.
  Stack s;
  s.push_back(IValue::fromTensor(iotaTensor(2, 4)));
  s.push_back(IValue::fromInt(0));
  runCpuOp<6>(s);
  ASSERT_EQ(1, s.size());
  EXPECT_EQ(DType::kLong, s.back().tensor()->dtype);
  EXPECT_EQ(longs, elems<uint64_t>(s.back(), 8));

  s.clear();
  s.push_back(IValue::fromTensor(largeTensor()));
  s.push_back(IValue::fromInt(0));
  runCpuOp<14>(s);
  ASSERT_EQ(1, s.size());
  EXPECT_EQ(DType::kHalf, s.back().tensor()->dtype);
  EXPECT_EQ(halves, elems<uint16_t>(s.back(), 8));
}

TEST(TorchDispatchImplTest, SliceAndSplitReturnViews) {
  // Slice op 2 with argument 0 takes rows [2, 4) of a 4-row tensor.
  Tensor x = iotaTensor(4, 8);
  Stack s;
  s.push_back(IValue::fromTensor(x));
  s.push_back(IValue::fromInt(0));
  runCpuOp<2>(s);
  ASSERT_EQ(1, s.size());
  EXPECT_EQ(x->storage.get(), s.back().tensor()->storage.get());
  EXPECT_EQ(2, s.back().tensor()->rows);
  EXPECT_EQ(x->bytes() + 2 * 8 * sizeof(float), s.back().tensor()->bytes());

  // Split op 7 with argument 1 cuts 6 rows into 3 views of 2 rows.
  Tensor y = iotaTensor(6, 4);
  s.clear();
  s.push_back(IValue::fromTensor(y));
  s.push_back(IValue::fromInt(1));
  runCpuOp<7>(s);
  ASSERT_EQ(1, s.size());
  ASSERT_TRUE(s.back().isList());
  std::vector<const char*> starts;
  for (const auto& part : s.back().list()->elems) {
    EXPECT_EQ(y->storage.get(), part.tensor()->storage.get());
    EXPECT_EQ(2, part.tensor()->rows);
    starts.push_back(part.tensor()->bytes());
  }
  const std::vector<const char*> expected = {
      y->bytes(), y->bytes() + 32, y->bytes() + 64};
  EXPECT_EQ(expected, starts);
}

TEST(TorchDispatchImplTest, IndexAndCatCopyIntoNewStorage) {
  // Index op 5 with argument 0 copies rows [2, 4) of a 4-row tensor.
  Tensor x = iotaTensor(4, 8);
  Stack s;
  s.push_back(IValue::fromTensor(x));
  s.push_back(IValue::fromInt(0));
  runCpuOp<5>(s);
  ASSERT_EQ(1, s.size());
  EXPECT_NE(x->storage.get(), s.back().tensor()->storage.get());
  EXPECT_EQ(2, s.back().tensor()->rows);
  EXPECT_EQ(iota(17, 16), elems(s.back(), 16));

  // Cat op 4 stacks the rows of its list and packs the words in order.
  auto* list = new ListImpl();
  list->elems.push_back(IValue::fromTensor(iotaTensor(2, 8)));
  list->elems.push_back(IValue::fromTensor(iotaTensor(3, 4)));
  s.clear();
  s.push_back(IValue::fromList(list));
  runCpuOp<4>(s);
  ASSERT_EQ(1, s.size());
  EXPECT_EQ(5, s.back().tensor()->rows);
  EXPECT_EQ(8, s.back().tensor()->cols);
  std::vector<uint32_t> expected = iota(1, 16);
  const std::vector<uint32_t> second = iota(1, 12);
  expected.insert(expected.end(), second.begin(), second.end());
  EXPECT_EQ(expected, elems(s.back(), 28));
}

TEST(TorchDispatchImplTest, InterpreterMovesValuesThroughListsAndDicts) {
  Code code;
  code.nregs = 4;
  code.ninputs = 1;
  code.constants.push_back(IValue::fromInt(1));
  OpEntry slice;
  slice.id = 2;
  slice.nargs = nargsOf(2);
  slice.work_cap = 64;
  slice.table[kCPU] = cpuKernels()[2];
  slice.table[kAutogradCPU] = &autogradKernel;
  code.ops.push_back(slice);
  code.instrs = {
      {OpCode::kLoad, 0, 0},
      {OpCode::kLoadConst, 0, 0},
      {OpCode::kOp, 0, 0},
      {OpCode::kStore, 0, 1},
      {OpCode::kLoad, 0, 0},
      {OpCode::kLoad, 0, 1},
      {OpCode::kListConstruct, 2, 0},
      {OpCode::kListUnpack, 3, 0},
      {OpCode::kDrop, 0, 0},
      {OpCode::kDictConstruct, 2, 10},
      {OpCode::kDictIndex, 0, 11},
      {OpCode::kStore, 0, 2},
      {OpCode::kMove, 0, 0},
      {OpCode::kStore, 0, 3},
  };

  Tensor input = iotaTensor(4, 8);
  TensorImpl* input_impl = input.get();
  std::vector<IValue> regs(code.nregs);
  regs[0] = IValue::fromTensor(std::move(input));
  Stack stack;
  run(code, regs, stack);

  EXPECT_TRUE(stack.empty());
  EXPECT_FALSE(regs[0].isTensor());
  ASSERT_TRUE(regs[1].isTensor());
  EXPECT_EQ(2, regs[1].tensor()->rows);
  EXPECT_EQ(input_impl->storage.get(), regs[1].tensor()->storage.get());
  EXPECT_EQ(regs[1].tensor(), regs[2].tensor());
  EXPECT_EQ(input_impl, regs[3].tensor());
  // The list and dict are gone, so only the registers hold references.
  EXPECT_EQ(1, input_impl->refcount.load());
  EXPECT_EQ(2, regs[1].tensor()->refcount.load());
}

TEST(TorchDispatchImplTest, OpIdsCoverEveryFamily) {
  for (int nops : {8, 100, 128, 256, 1000, 1024}) {
    GraphSpec g;
    g.nops = nops;
    g.ninstr_ops = 1;
    const Code code = buildCode(g);
    ASSERT_EQ(nops, code.ops.size());
    std::set<int> ids;
    std::set<int> families;
    for (const auto& op : code.ops) {
      EXPECT_LE(0, op.id);
      EXPECT_GT(kMaxOps, op.id);
      ids.insert(op.id);
      families.insert(familyOf(op.id));
    }
    EXPECT_EQ(nops, ids.size()) << "nops " << nops;
    EXPECT_EQ(kNumFamilies, families.size()) << "nops " << nops;
  }
  // With every operator in use, op i is kernel i.
  GraphSpec g;
  g.nops = kMaxOps;
  g.ninstr_ops = 1;
  std::vector<int> ids;
  for (const auto& op : buildCode(g).ops) {
    ids.push_back(op.id);
  }
  std::vector<int> expected(kMaxOps);
  std::iota(expected.begin(), expected.end(), 0);
  EXPECT_EQ(expected, ids);
}

TEST(TorchDispatchImplTest, GraphsDependOnlyOnSeed) {
  auto encode = [](const Code& code) {
    std::vector<uint32_t> out;
    out.reserve(code.instrs.size());
    for (const auto& in : code.instrs) {
      out.push_back(
          (static_cast<uint32_t>(in.code) << 24) |
          (static_cast<uint32_t>(in.n) << 16) | in.x);
    }
    return out;
  };
  GraphSpec g = measuredSpec();
  const auto first = encode(buildCode(g));
  EXPECT_EQ(first, encode(buildCode(g)));
  g.seed = 2;
  EXPECT_NE(first, encode(buildCode(g)));
}

TEST(TorchDispatchImplTest, MeasuredGraphRunsBalanced) {
  const GraphSpec g = measuredSpec();
  const Code code = buildCode(g);
  int nop_calls = 0;
  for (const auto& in : code.instrs) {
    nop_calls += OpCode::kOp == in.code;
  }
  EXPECT_EQ(g.ninstr_ops, nop_calls);
  int nmeta = 0;
  for (const auto& op : code.ops) {
    nmeta += op.meta;
    EXPECT_TRUE(op.shared_meta);
  }
  EXPECT_NEAR(g.meta_frac * g.nops, nmeta, 0.02 * g.nops);

  // Every op leaves exactly its result, so a run ends with an empty stack.
  std::vector<IValue> regs(code.nregs);
  for (int i = 0; code.ninputs > i; ++i) {
    regs[i] = IValue::fromTensor(iotaTensor(32, 64));
  }
  Stack stack;
  run(code, regs, stack);
  EXPECT_TRUE(stack.empty());

  const int64_t live = runOnce(code, 32, 64, regs, stack);
  EXPECT_LT(0, live);
  EXPECT_GE(code.nregs, live);
  EXPECT_TRUE(regs.empty());
  EXPECT_TRUE(stack.empty());
}

TEST(TorchDispatchImplTest, ChaseHeapIsOneCycle) {
  GraphSpec g;
  g.ninstr_ops = 1;
  g.chase_len = 1;
  g.chase_nodes = 1000;
  const Code code = buildCode(g);
  ASSERT_NE(nullptr, code.chase);
  const auto& nodes = *code.chase;
  std::vector<bool> seen(nodes.size());
  uint32_t n = 0;
  for (size_t step = 0; nodes.size() > step; ++step) {
    EXPECT_FALSE(seen[n]) << "node " << n << " revisited";
    seen[n] = true;
    n = nodes[n].next;
  }
  EXPECT_EQ(0, n);
  EXPECT_EQ(0, chaseMetadata(code.ops[0]));
}

TEST(TorchDispatchImplTest, RejectsInvalidSpecs) {
  GraphSpec g;
  g.ninstr_ops = 1;
  g.ninputs = 0;
  EXPECT_THROW(buildCode(g), std::invalid_argument);
  g.ninputs = kMaxRegs - 1;
  EXPECT_THROW(buildCode(g), std::invalid_argument);
  g.ninputs = 8;
  g.nregs = kMaxRegs + 1;
  EXPECT_THROW(buildCode(g), std::invalid_argument);
  g.nregs = kMaxRegs;
  EXPECT_EQ(kMaxRegs, buildCode(g).nregs);
}

} // namespace
} // namespace facebook::cea::chips::adsim::torch_dispatch
