namespace cpp2 ranking

cpp_include "folly/small_vector.h"
cpp_include "folly/container/F14Map.h"

include "thrift/annotation/cpp.thrift"

struct Payload {
  1: string message;
}

enum RankingStoryType {
  STORY_TYPE_A = 0,
  STORY_TYPE_B = 1,
  STORY_TYPE_C = 2,
  STORY_TYPE_D = 3,
  STORY_TYPE_E = 4,
  STORY_TYPE_F = 5,
  STORY_TYPE_G = 6,
  STORY_TYPE_H = 7,
  STORY_TYPE_I = 8,
  STORY_TYPE_J = 9,
  STORY_TYPE_K = 10,
  STORY_TYPE_L = 11,
  STORY_TYPE_M = 12,
  STORY_TYPE_N = 13,
  STORY_TYPE_O = 14,
  STORY_TYPE_P = 15,
  STORY_TYPE_Q = 16,
  STORY_TYPE_R = 17,
  STORY_TYPE_S = 18,
  STORY_TYPE_T = 19,
  STORY_TYPE_U = 20,
  STORY_TYPE_V = 21,
  STORY_TYPE_W = 22,
  STORY_TYPE_X = 23,
  STORY_TYPE_Y = 24,
  STORY_TYPE_Z = 25,
}

enum RankingObjectType {
  OBJ_TYPE_A = 0,
  OBJ_TYPE_B = 1,
  OBJ_TYPE_C = 2,
  OBJ_TYPE_D = 3,
  OBJ_TYPE_E = 4,
  OBJ_TYPE_F = 5,
  OBJ_TYPE_G = 6,
  OBJ_TYPE_H = 7,
  OBJ_TYPE_I = 8,
  OBJ_TYPE_J = 9,
  OBJ_TYPE_K = 10,
  OBJ_TYPE_L = 11,
  OBJ_TYPE_M = 12,
  OBJ_TYPE_N = 13,
  OBJ_TYPE_O = 14,
  OBJ_TYPE_P = 15,
  OBJ_TYPE_Q = 16,
  OBJ_TYPE_R = 17,
  OBJ_TYPE_S = 18,
  OBJ_TYPE_T = 19,
  OBJ_TYPE_U = 20,
  OBJ_TYPE_V = 21,
  OBJ_TYPE_W = 22,
  OBJ_TYPE_X = 23,
  OBJ_TYPE_Y = 24,
  OBJ_TYPE_Z = 25,
}

struct Action {
  1: i16 type;
  2: i64 timeUsec;
  3: i32 timeMsec;
  4: i64 actorID;
}

@cpp.Type{name = "folly::small_vector<int64_t, 8>"}
typedef list<i64> SmallListI64
@cpp.Type{template = "folly::F14FastMap"}
typedef map<i16, i64> RankingPayloadIntMap
@cpp.Type{template = "folly::F14FastMap"}
typedef map<i16, string> RankingPayloadStringMap
@cpp.Type{template = "folly::F14FastMap"}
typedef map<i16, SmallListI64> RankingPayloadVecMap

struct RankingObject {
  1: i64 objectID;
  2: RankingObjectType objectType;
  3: i64 actorID;
  4: i64 createTime;
  5: RankingPayloadIntMap payloadIntMap;
  6: RankingPayloadStringMap payloadStrMap;
  7: RankingPayloadVecMap payloadVecMap;
  8: list<Action> actions;
  9: double weight;
}

struct RankingStory {
  1: i64 storyID;
  2: list<RankingObject> objects;
  3: double weight;
  4: RankingStoryType storyType;
}

struct RankingResponse {
  1: i64 queryID;
  2: list<RankingStory> rankingStories;
  3: list<i32> objectCounts;
  4: string metadata;
}
