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

#include <cassert>
#include <string>

#include <glog/logging.h>

#include <folly/Synchronized.h>
#include <folly/container/F14Map.h>

#include <thrift/lib/cpp2/protocol/CompactProtocol.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <cea/chips/adsim/if/gen-cpp2/AdSim.h>

namespace facebook {
namespace cea {
namespace chips {
namespace adsim {

class AdSimHandler; // forward declaration

class AdSimBaseObjs {
 public:
  ~AdSimBaseObjs() {
    obj_map.withWLock([&](auto& objs) {
      for (auto& obj : objs) {
        if (NULL != obj.second) {
          free(obj.second);
        }
      }
    });
  }
  bool contains(const std::string& key) const {
    return obj_map.rlock()->contains(key);
  }
  void* get_plain_ptr(const std::string& key) const {
    void* ret = nullptr;
    obj_map.withRLock([&](auto const& objs) {
      auto iter = objs.find(key);
      if (objs.end() != iter) {
        ret = iter->second;
      }
    });
    return ret;
  }
  void set_plain_ptr(std::string key, void* FOLLY_NULLABLE ptr) {
    obj_map.wlock()->insert(std::pair<std::string, void*>(key.c_str(), ptr));
  }
  void* get_plain_ptr_default(
      const std::string& key,
      std::function<void*()> f) {
    void* ret = nullptr;
    obj_map.withWLock([&](auto& objs) {
      auto iter = objs.find(key);
      if (objs.end() == iter) {
        iter =
            objs.insert(std::pair<std::string, void*>(key.c_str(), f())).first;
      }
      ret = iter->second;
    });
    return ret;
  }

  template <typename T>
  std::shared_ptr<T> get_shared_ptr(const std::string& key) const {
    std::shared_ptr<T> ret;
    shared_obj_map.withRLock([&](auto const& objs) {
      auto iter = objs.find(key);
      if (objs.end() != iter) {
        ret = std::static_pointer_cast<T>(iter->second);
      }
    });
    return ret;
  }
  template <typename T>
  void set_shared_ptr(std::string key, std::shared_ptr<T> ptr) {
    shared_obj_map.wlock()->insert(
        std::pair<std::string, std::shared_ptr<T>>(key.c_str(), ptr));
  }
  template <typename T>
  std::shared_ptr<T> get_shared_ptr_default(
      const std::string& key,
      std::function<std::shared_ptr<T>()> f) {
    std::shared_ptr<T> ret;
    shared_obj_map.withWLock([&](auto& objs) {
      auto iter = objs.find(key);
      if (objs.end() == iter) {
        iter = objs.insert(std::pair<std::string, std::shared_ptr<T>>(
                               key.c_str(), f()))
                   .first;
      }
      ret = std::static_pointer_cast<T>(iter->second);
    });
    return ret;
  }

 protected:
  folly::Synchronized<folly::F14NodeMap<std::string, void*>> obj_map;
  folly::Synchronized<folly::F14NodeMap<std::string, std::shared_ptr<void>>>
      shared_obj_map;
};

/* This class contains global objects for one particular instance of AdSim
 * server.
 */
class AdSimGlobalObjs : public AdSimBaseObjs {
 public:
  static std::shared_ptr<AdSimGlobalObjs> get();
};

/* This class contains objects local to every handler thread.
 */
class AdSimHandleObjs : public AdSimBaseObjs {};

/* This class contains objects local to every request.
 */
class AdSimRequestObjs : public AdSimBaseObjs {
 public:
  explicit AdSimRequestObjs(std::unique_ptr<AdSimRequest> req)
      : AdSimBaseObjs(), request(std::move(req)) {
    int r = clock_gettime(CLOCK_MONOTONIC, &ts_);
    PCHECK(0 == r);
  }
  std::string request_str() const {
    return *request->request();
  }
  int64_t receive_us() const {
    return (ts_.tv_nsec / 1000 + ts_.tv_sec * 1000000);
  }

 private:
  std::unique_ptr<AdSimRequest> request;
  struct timespec ts_;
};

} // namespace adsim
} // namespace chips
} // namespace cea
} // namespace facebook
