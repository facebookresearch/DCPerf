namespace cpp2 ranking

include "thrift/annotation/cpp.thrift"
include "thrift/annotation/thrift.thrift"

@thrift.AllowLegacyMissingUris
package;

cpp_include "folly/small_vector.h"
cpp_include "folly/container/F14Map.h"

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

// ============================================================================
// Phase 7: Client-Side Feature Generation Types
// ============================================================================

// Dense features are floating-point values (e.g., normalized continuous features)
typedef list<double> DenseFeatureVector

// Sparse features are integer indices into embedding tables
typedef list<i64> SparseFeatureVector

// DLRM features structure for client-side feature generation
struct DLRMFeatures {
  1: DenseFeatureVector dense_features;
  2: SparseFeatureVector sparse_features;
  3: i32 batch_size;
  4: i32 num_dense_features;
  5: i32 num_sparse_features;
}

// ============================================================================
// Phase 3 (Plan): Client-Server Story Content Types (Silesia Dataset)
// ============================================================================

// A single story containing raw bytes from the Silesia corpus
struct StoryContent {
  1: i64 story_id;
  2: binary content;        // Raw bytes from Silesia files
  3: string source_file;    // Which Silesia file this snippet came from
  4: i32 content_length;    // Length of content in bytes
}

// A batch of stories sent per request
struct StoryBatch {
  1: list<StoryContent> stories;
}

// Request structure for DLRM inference with client-provided features
struct RankingRequest {
  1: i64 request_id;
  2: optional DLRMFeatures dlrm_features;
  3: i32 num_inferences = 1;
  4: map<string, string> metadata;
  5: optional StoryBatch story_batch;
  // Opaque padding to control serialized request size (matches prod size dist).
  6: optional binary padding;
}

// ============================================================================
// Phase 4: Production-shaped Multifeed Aggregator inbound methods.
//
// These structs mirror the field counts and types of the prod multifeed
// aggregator inbound methods (createAndPrimeSession, getStoriesUncompressed,
// getAllStories, streamData, streamIfrPriorityRanking). Each struct includes
// a `binary` padding/payload field whose default size is sized so that
// `apache::thrift::CompactSerializer::serialize` lands within +/-10% of the
// p50 wire size from `~/feedsim_v2/profiles/rpc_dist.json`. Callers (Phase 6
// driver) sample actual sizes from the percentile table and fill the binary
// fields accordingly.
//
// Existing `RankingRequest`/`RankingResponse` are kept until Phase 6 deletes
// them.
// ============================================================================

// p50 wire size target: 379 bytes
struct CreateAndPrimeSessionRequest {
  1: i64 user_id;
  2: i64 query_id;
  3: string caller_id;
  4: string source;
  5: string locale;
  6: string client_query_id;
  7: i32 platform_type;
  8: i32 browser_type;
  9: bool is_employee;
  10: i32 frontend_recv_timeout = 2500;
  // Bulk padding: prod priming-request payload includes create_session_request
  // and prime_session_request blobs. Sized to hit p50.
  11: binary session_init_blob;
}

// p50 wire size target: 44 bytes
struct CreateAndPrimeSessionResponse {
  1: string session_id;
  2: i32 status_code;
}

// Helper: shared response stats for getStoriesUncompressed and getAllStories.
struct GetStoriesResponseStats {
  1: i32 num_actions_received;
  2: i32 num_friends_queried;
  3: i32 num_object_summaries_received;
  4: i32 num_ads_received;
  5: i32 num_creation_action;
  6: i32 num_actions_point_query;
  7: i32 num_actions_graph_query;
  8: i32 num_big_object_summaries_received;
  9: i32 friend_inventory_count;
  10: i32 page_inventory_count;
  11: i32 group_inventory_count;
  12: i32 num_stale_organic_stories;
}

// Helper: per-story payload for getStoriesUncompressed/getAllStories
// responses. story_payload sized so ~100 stories x ~1.5 KB matches the
// 171 KB p50 of getStoriesUncompressed.
struct RankedStoryInfo {
  1: i64 story_key;
  2: i64 actor_id;
  3: i64 target_id;
  4: i64 object_id;
  5: i32 source_type;
  6: i32 story_type;
  7: i32 time_published;
  8: double weight;
  9: double weight_user;
  10: double weight_participants;
  11: double weight_event;
  12: double discounted_weight;
  13: binary story_payload;
}

// p50 wire size target: 2,127,642 bytes (~2.13 MB)
struct GetStoriesRequest {
  1: string session_id;
  2: i64 query_id;
  3: i64 user_id;
  4: string caller_id;
  5: string source;
  6: string locale;
  7: string mobile_app_version;
  8: i32 platform_type;
  9: i32 browser_type;
  10: i32 frontend_recv_timeout = 2500;
  11: bool log_for_ranking;
  12: bool is_employee;
  13: i64 caller_app_id;
  14: i32 nth_retry = 0;
  15: string expected_ranking_model;
  16: string push_phase;
  17: i32 high_busy_contexts;
  // ~1.5 MB compressed user settings (largest field by size in prod).
  18: binary settings_compressed;
  // ~500 KB poseidon UP2/UIH packed row data.
  19: binary poseidon_data2;
  // List of UIH actions (typed list of small structs in prod) — collapsed
  // to a single binary blob to match wire size without rebuilding 70+
  // struct types.
  20: binary uih_actions_serialized;
  // Smaller blob: caller-supplied feed bank data, query parameters,
  // backend dispatcher params (combined ~50 KB).
  21: binary backend_dispatcher_params;
  22: binary stories_query;
  23: list<i64> needy_user_recommendation_actors;
}

// p50 wire size target: 171,393 bytes (~171 KB)
struct GetStoriesResponse {
  1: i64 query_id;
  2: i32 status_code;
  3: GetStoriesResponseStats stats;
  // Dominant field: ~100 ranked stories, each ~1.5 KB.
  4: list<RankedStoryInfo> story_infos;
  5: binary edge_summaries_serialized;
  6: binary leaf_request_debug;
  7: binary view_state_diff;
}

// p50 wire size target: 55 bytes
struct GetAllStoriesRequest {
  1: string session_id;
  2: i64 query_id;
  3: string caller_id;
}

// p50 wire size target: 1,474,443 bytes (~1.47 MB)
// Bigger than getStoriesUncompressed because it includes ALL stories
// (not just first batch).
struct GetAllStoriesResponse {
  1: i64 query_id;
  2: i32 status_code;
  3: GetStoriesResponseStats stats;
  // ~500-1000 ranked stories at ~1.5 KB each.
  4: list<RankedStoryInfo> all_story_infos;
  5: binary aggregator_debug;
  6: binary edge_summaries_serialized;
  7: binary view_state_diff;
}

// Streaming use cases (driver tags streamData calls with one of these).
enum StreamingUseCase {
  TOP_SLOT_QP = 0,
  AUCTIONABLE_QP = 1,
  PYMK_QP = 2,
  FB_STORIES_TRAY = 3,
  FB_SHORTS = 4,
  RANKING_DATA = 5,
  IFR = 6,
  IFR_PRIORITY_RANKING = 7,
  CONNECTED_STORIES = 8,
  UNCONNECTED_STORIES = 9,
}

// p50 wire size target: 57,838 bytes (~58 KB; bimodal in prod, p75 ≈ 3.2 MB).
// Driver samples actual size per-request.
struct StreamDataRequest {
  1: StreamingUseCase use_case;
  2: string session_id;
  3: i64 request_id;
  4: i64 query_id;
  // Bulk: serialized thrift payload (compressed in prod).
  5: binary serialized_payload;
  6: binary streaming_metadata;
}

// p50 wire size target: 4 bytes (essentially empty ack).
struct StreamDataResponse {
  1: i32 ack_code;
}

// p50 wire size target: 948,545 bytes (~949 KB; very wide distribution).
struct StreamIfrPriorityRankingRequest {
  1: string session_id;
  2: i64 request_id;
  // Bulk: serialized list of RecAggObject in prod.
  3: binary ifr_objects_serialized;
  // execution_graph.RunResponse blob.
  4: binary eg_response_serialized;
  // shots.EvalResponse blob.
  5: binary eval_response_serialized;
  6: string eg_config_identifier;
  7: string ifr_request_mode_www;
}

// p50 wire size target: 4 bytes (essentially empty ack).
struct StreamIfrPriorityRankingResponse {
  1: i32 ack_code;
}
