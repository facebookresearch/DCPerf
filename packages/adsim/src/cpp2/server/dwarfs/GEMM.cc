/*
 * Copyright (c) Facebook, Inc. and its affiliates.
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

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <vector>

#include <glog/logging.h>

#ifdef _OPENMP
#endif

#ifdef USE_MKL
#endif

#include "bench/BenchUtils.h"
#include "fbgemm/Fbgemm.h"
#include "test/QuantizationHelpers.h"

#include <cea/chips/adsim/cpp2/server/dwarfs/GEMM.h>
#include <cea/chips/adsim/cpp2/server/dwarfs/Kernel.h>

using namespace std;
using namespace fbgemm;

namespace facebook::cea::chips::adsim {

/* Initialize the matrices
 *
 * @param  $h_objs  Handler objects
 * @param  $r_objs  Request objects
 */
std::string GEMM::init(
    std::shared_ptr<AdSimHandleObjs> h_objs,
    std::shared_ptr<AdSimRequestObjs> r_objs) {
  Kernel::init(h_objs, r_objs);
  auto Aint8 = std::make_shared<aligned_vector<uint8_t>>(M_ * K_);
  auto Bint8 = std::make_shared<aligned_vector<int8_t>>(K_ * N_);
  std::generate(Aint8->begin(), Aint8->end(), [&]() {
    uint8_t val = get_rand() % 6;
    return val;
  });
  std::generate(Bint8->begin(), Bint8->end(), [&]() {
    int8_t val = (get_rand() % 9) - 4;
    return val;
  });
  avoidOverflow(M_, N_, K_, Aint8->data(), Bint8->data());
  h_objs->set_shared_ptr(input_str_ + ".Aint8", Aint8);
  h_objs->set_shared_ptr(input_str_ + ".Bint8", Bint8);

  auto packedB_int32 = std::make_shared<PackBMatrix<int8_t>>(
      matrix_op_t::NoTranspose, K_, N_, Bint8->data(), N_, nullptr, 1);
  h_objs->set_shared_ptr(input_str_ + ".packedB", packedB_int32);

  auto Cint32 = std::make_shared<aligned_vector<int32_t>>(M_ * N_);
  h_objs->set_shared_ptr(input_str_ + ".Cint32", Cint32);
  return folly::to<std::string>(
      "GEMM: ",
      input_str_,
      " ",
      std::to_string(M_ * K_ + K_ * N_ + M_ * N_ + sizeof(packedB_int32)),
      "B");
}

/* Perform GEMM computation using fbgemm library
 *
 * @param  $shape  The size of matrices participating in the GEMM
 * @param  $niter  Number of iterations
 */
void GEMM::performance_test(
    const int niter,
    std::shared_ptr<AdSimHandleObjs> h_objs) {
  auto Aint8 =
      h_objs->get_shared_ptr<aligned_vector<uint8_t>>(input_str_ + ".Aint8");
  auto packedB = h_objs->get_shared_ptr<fbgemm::PackBMatrix<int8_t>>(
      input_str_ + ".packedB");
  auto Cint32 =
      h_objs->get_shared_ptr<aligned_vector<int32_t>>(input_str_ + ".Cint32");

  DoNothing<int32_t, int32_t> doNothing32BitObj;
  memCopy<> memcopyObj(doNothing32BitObj);
  for (auto i = 0; i < niter; ++i) {
    PackAMatrix<uint8_t> packA_int32(
        matrix_op_t::NoTranspose, M_, K_, Aint8->data(), K_, nullptr, 1);

    fbgemmPacked(
        packA_int32,
        *packedB.get(),
        Cint32->data(),
        Cint32->data(),
        N_,
        memcopyObj,
        0,
        1);
  }
}

} // namespace facebook::cea::chips::adsim
