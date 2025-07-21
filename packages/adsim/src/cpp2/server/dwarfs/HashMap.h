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

#pragma once

#include <unistd.h>

#include <algorithm>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include <cea/chips/adsim/cpp2/server/DataObjects.h>
#include <cea/chips/adsim/cpp2/server/dwarfs/Kernel.h>

#include <folly/Synchronized.h>
#include <folly/container/F14Map.h>
#include <folly/coro/Task.h>
#include <folly/sorted_vector_types.h>

namespace facebook {
namespace cea {
namespace chips {
namespace adsim {

/* A kernel does hash map operations intensively */
class HashMap : public Kernel {
  template <class K, class V>
  using unord = std::unordered_map<K, V>;
  template <class K, class V>
  using f14fast = folly::F14FastMap<K, V>;
  template <class K, class V>
  using f14val = folly::F14ValueMap<K, V>;
  template <class K, class V>
  using f14node = folly::F14NodeMap<K, V>;
  template <class K, class V>
  using f14vec = folly::F14VectorMap<K, V>;

 public:
  explicit HashMap(
      int runs,
      int size,
      std::string map_type,
      std::string input_str);

  std::string init(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs = nullptr) override;

  folly::coro::Task<std::string> fire(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs,
      std::shared_ptr<folly::Executor> pool) override;

  static std::shared_ptr<Kernel> config(const folly::dynamic& config_d) {
    int runs = config_d["runs"].asInt();
    int size = config_d["size"].asInt();
    std::string map_type = "f14val";
    std::string input_str = "Hhashmap";
    if (config_d.count("map_type")) {
      map_type = config_d["map_type"].asString();
    }
    if (config_d.count("input")) {
      input_str = config_d["input"].asString();
    }
    return std::make_shared<HashMap>(runs, size, map_type, input_str);
  }

 private:
  int runs_;
  int size_;
  std::string map_type_;
  std::string input_str_;

  folly::Synchronized<int>& keyListLock(); // defined in HashMap.cc

  template <template <class, class> class M>
  void insert_map(std::shared_ptr<AdSimHandleObjs> h_objs);

  template <template <class, class> class M>
  int lookup_map(std::shared_ptr<AdSimHandleObjs> h_objs);

  // The follows are from HashMapsBench
  const int kSalt = 0x619abc7e;

  template <typename K>
  K keyGen(uint32_t key) {
    return folly::to<K>(folly::hash::jenkins_rev_mix32(key ^ kSalt));
  }

  template <class K>
  std::vector<K>& keyList(int /*max*/ = 0); // defined in HashMap.cc

  template <class K>
  void prepare(int max) {
    auto& keys = keyList<K>();
    if (keys.size() < max) {
      for (int key = keys.size(); key < max; ++key) {
        keys.push_back(keyGen<K>(key));
      }
    }
  }

  template <class K>
  const K& key(int index) {
    return keyList<K>()[index];
  }

  template <template <class, class> class Map, class K, class V, class Test>
  void
  benchmarkFilledMapByKey(int runs, int size, const Test& test, int div = 1) {
    // removed BenchmarkSuspender, as we do not need timing
    Map<K, V> map(size);
    std::vector<K> toInsert;
    prepare<K>(size);
    for (int i = 0; i < size; ++i) {
      auto& k = key<K>(i);
      toInsert.push_back(k);
    }
    for (int i = 0; i * div < size; ++i) {
      map.insert(std::pair<K, V>(toInsert[i], i * 3));
    }
    std::random_shuffle(toInsert.begin(), toInsert.end());
    for (int r = 0; r < runs; ++r) {
      for (int i = 0; i < size; i += div) {
        test(map, toInsert[i]);
      }
    }
  }

  template <template <class, class> class Map, class K, class V>
  void benchmarkFind(int runs, int size) {
    int x = 0;
    benchmarkFilledMapByKey<Map, K, V>(
        runs,
        size,
        [&](Map<K, V>& m, const K& key) {
          auto found = m.find(key);
          if (found != m.end()) {
            x ^= found->second;
          }
        },
        2);
  }
};
} // namespace adsim
} // namespace chips
} // namespace cea
} // namespace facebook
