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

// MKL runs its AVX-512 kernels only on CPUs its check reports as Intel. With
// MKL linked into adsim_server_torch (ADSIM_STATIC_TORCH), the link wraps that
// check (--wrap=mkl_serv_intel_cpu_true) so AMD CPUs run them too.
extern "C" int __wrap_mkl_serv_intel_cpu_true() {
  return 1;
}
