// Copyright 2015 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef CEREAL_MAP_AS_JS_OBJECT_H
#define CEREAL_MAP_AS_JS_OBJECT_H

#include <cereal/archives/json.hpp>
#include <string>
#include <map>

// Clever hack to make cereal serialize a map as a JavaScript object
// Answer adopted from
// https://stackoverflow.com/questions/22569832/is-there-a-way-to-specify-a-simpler-json-de-serialization-for-stdmap-using-c
namespace cereal {
//! Saving for std::map<std::string, T>
template <class Archive, class T, class C, class A>
inline void save(Archive& ar, const std::map<std::string, T, C, A>& map) {
  for (const auto& i : map) {
    ar(cereal::make_nvp(i.first, i.second));
  }
}

//! Saving for std::map<uint32_t, T>
template <class Archive, class T, class C, class A>
inline void save(Archive& ar, const std::map<uint32_t, T, C, A>& map) {
  for (const auto& i : map) {
    std::stringstream ss;
    ss << i.first;
    ar(cereal::make_nvp(ss.str(), i.second));
  }
}
}  // namespace cereal

#endif  // CEREAL_MAP_AS_JS_OBJECT_H

