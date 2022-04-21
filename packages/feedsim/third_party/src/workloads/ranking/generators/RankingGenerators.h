// Copyright (c) 2019-present, Facebook, Inc. and its affiliates.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#ifndef RANKING_GENERATORS_H
#define RANKING_GENERATORS_H

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <vector>

#include <oldisim/Util.h>

#include <ranking/if/gen-cpp2/ranking_types.h>

namespace ranking {
namespace generators {

// Fast random number-generator
// Not cryptographically-safe
inline uint64_t xor128() {
  thread_local static uint64_t x = 123456789;
  thread_local static uint64_t y = 362436069;
  thread_local static uint64_t z = 521288629;
  thread_local static uint64_t w = 88675123;
  uint64_t t;
  t = x ^ (x << 11);
  x = y;
  y = z;
  z = w;
  return w = w ^ (w >> 19) ^ (t ^ (t >> 8));
}

inline ranking::Payload generateRandomPayload(size_t length) {
  ranking::Payload payload;
  payload.message = RandomString(length);
  return payload;
}

inline ranking::Action generateRandomAction() {
  ranking::Action action;
  uint64_t rand_int = static_cast<uint64_t>(xor128());

  action.type = static_cast<int16_t>(rand_int >> 48);
  action.timeUsec = static_cast<int64_t>(rand_int);
  action.timeMsec = static_cast<int32_t>(rand_int >> 32);
  action.actorID = static_cast<int64_t>(rand_int);
  return action;
}

inline ranking::RankingPayloadIntMap generateRandomIntMap(size_t length) {
  auto generate_int_map_pair = []() {
    auto k = static_cast<int16_t>(xor128());
    auto v = static_cast<int16_t>(42);
    return std::make_pair(k, v);
  };

  ranking::RankingPayloadIntMap map;
  map.reserve(length);
  std::generate_n(std::inserter(map, map.begin()), length,
                  generate_int_map_pair);
  return map;
}

inline ranking::RankingPayloadStringMap generateRandomStringMap(size_t length) {
  auto generate_str_map_pair = []() {
    auto k = static_cast<int16_t>(xor128());
    auto v = "abcdefghijklmnopqrstuvwyz";
    return std::make_pair(k, v);
  };
  ranking::RankingPayloadStringMap map;
  map.reserve(length);
  std::generate_n(std::inserter(map, map.begin()), length,
                  generate_str_map_pair);
  return map;
}

inline ranking::RankingPayloadVecMap generateRandomVecMap(size_t length) {
  auto generate_vec_map_pair = []() {
    ranking::SmallListI64 l;
    l.reserve(10);
    auto k = static_cast<int16_t>(xor128());
    auto v = l;
    return std::make_pair(k, v);
  };
  ranking::RankingPayloadVecMap map;
  map.reserve(length);
  std::generate_n(std::inserter(map, map.begin()), length,
                  generate_vec_map_pair);
  return map;
}

inline ranking::RankingObject
generateRandomRankingObject(size_t actions_length) {
  ranking::RankingObject obj;
  uint64_t rand_int = static_cast<uint64_t>(xor128());
  obj.objectID = static_cast<int64_t>(rand_int);
  obj.objectType = static_cast<ranking::RankingObjectType>(
      rand_int % static_cast<uint64_t>(ranking::RankingObjectType::OBJ_TYPE_Z));
  obj.actorID = static_cast<int64_t>(rand_int);
  obj.createTime = static_cast<int64_t>(rand_int);

  // FIXME(cltorres): Populate with realistic sizes
  obj.payloadIntMap = generateRandomIntMap(5);
  obj.payloadStrMap = generateRandomStringMap(5);
  obj.payloadVecMap = generateRandomVecMap(5);

  obj.actions.reserve(actions_length);
  std::generate_n(std::back_inserter(obj.actions), actions_length,
                  generateRandomAction);
  obj.weight = static_cast<double>(rand_int);

  return obj;
}

inline ranking::RankingStory
generateRandomRankingStory(size_t ranking_objects_length) {
  ranking::RankingStory story;
  uint64_t rand_int = static_cast<uint64_t>(xor128());
  story.storyID = static_cast<int64_t>(rand_int);
  story.objects.reserve(ranking_objects_length);
  // TODO(cltorres): Determine distribution of Actions per ranking object
  std::generate_n(std::back_inserter(story.objects), ranking_objects_length,
                  std::bind(generateRandomRankingObject, 5));
  story.weight = static_cast<double>(rand_int);
  story.storyType = static_cast<ranking::RankingStoryType>(
      rand_int %
      static_cast<uint64_t>(ranking::RankingStoryType::STORY_TYPE_Z));
  return story;
}

inline ranking::RankingResponse
generateRandomRankingResponse(size_t ranking_stories_length) {
  ranking::RankingResponse resp;
  uint64_t rand_int = static_cast<uint64_t>(xor128());
  resp.queryID = static_cast<int64_t>(rand_int);
  resp.rankingStories.reserve(ranking_stories_length);
  // TODO(cltorres): Determine distribution of ranking objects per story
  std::generate_n(std::back_inserter(resp.rankingStories),
                  ranking_stories_length,
                  std::bind(generateRandomRankingStory, 20));
  resp.objectCounts.reserve(ranking_stories_length);
  resp.metadata = RandomString(200);
  return resp;
}

} // namespace generators
} // namespace ranking
#endif
