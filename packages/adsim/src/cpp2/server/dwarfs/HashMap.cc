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

#include <cea/chips/adsim/cpp2/server/dwarfs/HashMap.h>

#include <fb303/ThreadCachedServiceData.h>

namespace facebook::cea::chips::adsim {

DECLARE_timeseries(hashmap_nfired);

/* A closure to create a vector of keys of certain type
 *
 * @return  The reference to the vector of keys
 */
template <class K>
std::vector<K>& HashMap::keyList(int /*max*/) {
  static std::vector<K> keys;
  return keys;
}

/* A closure to provide the lock for keylist of certain type
 *
 * @return  The reference to the lock
 */
folly::Synchronized<int>& HashMap::keyListLock() {
  static folly::Synchronized<int> key_list_lock;
  return key_list_lock;
}

/* HashMap kernel constructor
 *
 * @param  $runs  Number of iterations per fire
 * @param  $size  Number of elements in the map
 * @param  $map_type  Map type (choose from unord, f14fast, f14val, f14node,
 *                    f14vec)
 * @param  $input_str  Input string
 * @return  A HashMap instance.
 */
HashMap::HashMap(
    int runs,
    int size,
    std::string map_type,
    std::string input_str)
    : runs_(runs), size_(size), map_type_(map_type), input_str_(input_str) {}

/* Helper function to create specific map in the handler objects
 *
 * @param  $h_objs  Handler objects
 */
template <template <class, class> class M>
void HashMap::insert_map(std::shared_ptr<AdSimHandleObjs> h_objs) {
  auto m = std::make_shared<M<std::string, uint8_t>>(size_);
  for (int i = 0; i < size_; ++i) {
    m->insert(std::pair<std::string, uint8_t>(key<std::string>(i), i * 3));
  }
  h_objs->set_shared_ptr(input_str_, m);
}

/* Initialize kernel dataset, the map
 *
 * @param  $h_objs  Handler objects
 * @param  $r_objs  Reuqest objects
 */
std::string HashMap::init(
    std::shared_ptr<AdSimHandleObjs> h_objs,
    std::shared_ptr<AdSimRequestObjs> /*unused*/) {
  keyListLock().withWLock(
      [&](int& /*unused*/) { prepare<std::string>(size_); });
  if ("unord" == map_type_) {
    insert_map<unord>(h_objs);
  } else if ("f14fast" == map_type_) {
    insert_map<f14fast>(h_objs);
  } else if ("f14val" == map_type_) {
    insert_map<f14val>(h_objs);
  } else if ("f14node" == map_type_) {
    insert_map<f14node>(h_objs);
  } else if ("f14vec" == map_type_) {
    insert_map<f14vec>(h_objs);
  }
  return folly::to<std::string>(
      "HashMap: ", input_str_, " ", std::to_string(size_), "B");
}

/* Helper function to lookup specific map in the handler objects
 *
 * @param  $h_objs  Handler objects
 */
template <template <class, class> class M>
int HashMap::lookup_map(std::shared_ptr<AdSimHandleObjs> h_objs) {
  auto m = h_objs->get_shared_ptr<M<std::string, uint8_t>>(input_str_);
  int x = 0;
  for (int r = 0; r < runs_; ++r) {
    for (int i = 0; i < size_; i += 2) {
      auto found = m->find(key<std::string>(i));
      if (found != m->end()) {
        x ^= found->second;
      }
    }
  }
  return x;
}

/* Invoke HashMap kernel
 *
 * @param  $req  Request string (content doesn't matter)
 * @return  A HashMap coroutine task
 */
folly::coro::Task<std::string> HashMap::fire(
    std::shared_ptr<AdSimHandleObjs> h_objs,
    std::shared_ptr<AdSimRequestObjs> r_objs,
    std::shared_ptr<folly::Executor> pool) {
  STATS_hashmap_nfired.add(1);
  /// inc_nfired("HashMap");
  /// DLOG_EVERY_N(INFO, 1000) << "HashMap fired " << get_nfired()["HashMap"];
  if ("unord" == map_type_) {
    lookup_map<unord>(h_objs);
  } else if ("f14fast" == map_type_) {
    lookup_map<f14fast>(h_objs);
  } else if ("f14val" == map_type_) {
    lookup_map<f14val>(h_objs);
  } else if ("f14node" == map_type_) {
    lookup_map<f14node>(h_objs);
  } else if ("f14vec" == map_type_) {
    lookup_map<f14vec>(h_objs);
  }
  co_return "";
}

} // namespace facebook::cea::chips::adsim
