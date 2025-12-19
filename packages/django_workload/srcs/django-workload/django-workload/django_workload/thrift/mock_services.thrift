// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// Mock Thrift service for DjangoBench V2 - SIMPLIFIED VERSION
// Minimal structures for performance

namespace py mock_services

// ============================================================================
// SIMPLIFIED Ad Insertion Struct (~30 fields for performance)
// ============================================================================

struct AdInsertion {
  // Core ad identifiers (10 fields)
  1: i64 ad_id;
  2: i64 campaign_id;
  3: i64 creative_id;
  4: i64 advertiser_id;
  5: string tracking_token;
  6: string impression_id;
  7: string ad_title;
  8: string ad_subtitle;
  9: string call_to_action;
  10: string destination_url;

  // Engagement metrics (5 fields)
  11: i64 view_count;
  12: i64 like_count;
  13: i32 comment_count;
  14: i32 share_count;
  15: bool is_video;

  // Ranking scores (10 fields)
  16: double quality_score;
  17: double predicted_ctr;
  18: double predicted_cvr;
  19: double relevance_score;
  20: double engagement_score;
  21: double brand_safety_score;
  22: double user_affinity_score;
  23: double content_quality_score;
  24: double viewability_score;
  25: double completion_rate;

  // Media info (5 fields)
  26: string image_url;
  27: string media_type;
  28: i32 video_duration;
  29: string surface_type;
  30: string placement_type;
}

// ============================================================================
// Request/Response for Ads Service
// ============================================================================

struct FetchAdsRequest {
  1: i64 user_id;
  2: i32 num_ads_requested;
  3: string surface_type;
  4: optional map<string, string> context;
}

struct FetchAdsResponse {
  1: list<AdInsertion> ads;
  2: i32 total_fetched;
  3: string request_id;
}

// ============================================================================
// Ranking Service Structs
// ============================================================================

service MockRankingService {
  RankItemsResponse rankItems(1: RankItemsRequest request);
}

struct RankItemsRequest {
  1: i64 user_id;
  2: list<string> item_ids;
  3: i32 num_results;
}

struct RankItemsResponse {
  1: list<string> item_ids;
  2: list<double> scores;
  3: string request_id;
}

service MockAdsService {
  FetchAdsResponse fetchAds(1: FetchAdsRequest request);
}

// ============================================================================
// Content Filtering Service
// ============================================================================

service MockContentFilterService {
  FilterContentResponse filterContent(1: FilterContentRequest request);
}

struct FilterContentRequest {
  1: i64 user_id;
  2: list<string> item_ids;
  3: string filter_level;
}

struct FilterContentResponse {
  1: list<string> safe_item_ids;
  2: list<string> blocked_item_ids;
  3: i32 total_filtered;
  4: string request_id;
}

// ============================================================================
// User Preference Service
// ============================================================================

service MockUserPreferenceService {
  UserPreferencesResponse getUserPreferences(1: UserPreferencesRequest request);
}

struct UserPreferencesRequest {
  1: i64 user_id;
}

struct UserPreferencesResponse {
  1: map<string, double> preferences;
  2: list<string> favorite_topics;
  3: string request_id;
}

// ============================================================================
// Clips Discovery Service - Models clips.api.views.async_stream_clips_discover
// ============================================================================

service MockClipsDiscoverService {
  ClipsDiscoverResponse discoverClips(1: ClipsDiscoverRequest request);
  ClipsRankingResponse rankClips(1: ClipsRankingRequest request);
  ClipsChunksResponse getClipsChunks(1: ClipsChunksRequest request);
}

struct ClipMedia {
  1: i64 clip_id;
  2: i64 owner_id;
  3: string title;
  4: string description;
  5: i32 duration_ms;
  6: i64 view_count;
  7: i64 like_count;
  8: i32 comment_count;
  9: i32 share_count;
  10: string thumbnail_url;
  11: string content_type;
  12: double quality_score;
  13: double engagement_score;
  14: list<string> hashtags;
  15: bool is_ad;
}

struct ClipChunk {
  1: i64 chunk_id;
  2: i64 video_id;
  3: i32 chunk_index;
  4: string chunk_url;
  5: i32 chunk_size_bytes;
  6: i32 duration_ms;
  7: i32 start_time_ms;
  8: i32 end_time_ms;
  9: string resolution;
  10: i32 bitrate_kbps;
}

struct ClipsDiscoverRequest {
  1: i64 user_id;
  2: i32 num_clips_requested;
  3: optional string max_id;
  4: optional list<string> seen_reels;
  5: string container_module;
  6: bool include_ads;
}

struct ClipsDiscoverResponse {
  1: list<ClipMedia> clips;
  2: list<AdInsertion> ads;
  3: i32 total_clips;
  4: string next_max_id;
  5: bool more_available;
  6: string request_id;
}

struct ClipsRankingRequest {
  1: i64 user_id;
  2: list<i64> clip_ids;
  3: i32 num_results;
  4: string ranking_model;
}

struct ClipsRankingResponse {
  1: list<i64> ranked_clip_ids;
  2: list<double> scores;
  3: string request_id;
}

struct ClipsChunksRequest {
  1: i64 video_id;
  2: i32 start_chunk;
  3: i32 num_chunks;
  4: string resolution;
}

struct ClipsChunksResponse {
  1: list<ClipChunk> chunks;
  2: i32 total_chunks;
  3: string request_id;
}

// ============================================================================
// Reels Tray Service - Models feed.api.views.reels_tray
// ============================================================================

service MockReelsTrayService {
  ReelsTrayResponse getTray(1: ReelsTrayRequest request);
  TrayRankingResponse rankTrayUsers(1: TrayRankingRequest request);
  UserMetadataBatchResponse getUserMetadataBatch(
    1: UserMetadataBatchRequest request,
  );
  TrayBucketClipsResponse getTrayBucketClips(1: TrayBucketClipsRequest request);
}

// User metadata for tray bucket display (NodeAPI/LazyUserDict pattern)
struct TrayUserMetadata {
  1: i64 user_id;
  2: string username;
  3: string full_name;
  4: string profile_pic_url;
  5: bool is_verified;
  6: bool has_unseen_stories;
  7: i32 story_count;
  8: i32 reel_count;
  9: bool is_live;
  10: i64 latest_reel_timestamp;
  11: bool is_close_friend;
  12: bool is_favorite;
  13: double affinity_score;
  14: bool has_besties_media;
  15: string ring_color;
}

// A single bucket in the tray (represents one user's stories/reels)
struct TrayBucket {
  1: i64 bucket_id;
  2: i64 user_id;
  3: TrayUserMetadata user_metadata;
  4: list<TrayReelItem> items;
  5: i32 item_count;
  6: bool is_materialized;
  7: i64 seen_at;
  8: double ranking_score;
  9: i32 position;
  10: string bucket_type;
}

// A single reel/story item within a bucket
struct TrayReelItem {
  1: i64 item_id;
  2: i64 owner_id;
  3: string media_type;
  4: i32 duration_ms;
  5: string thumbnail_url;
  6: string video_url;
  7: i64 taken_at;
  8: i64 expiring_at;
  9: bool is_seen;
  10: i64 seen_at;
  11: i32 view_count;
  12: i32 reply_count;
  13: bool has_audio;
  14: string audio_track_id;
  15: list<string> hashtags;
}

// Paging info for infinite scroll
struct TrayPagingInfo {
  1: string max_id;
  2: bool more_available;
  3: i32 prefetch_count;
  4: string next_cursor;
}

// Main tray request
struct ReelsTrayRequest {
  1: i64 viewer_id;
  2: i32 num_buckets_requested;
  3: optional string max_id;
  4: bool include_live;
  5: bool include_self_story;
  6: i32 num_items_per_bucket;
  7: string source_module;
  8: optional list<i64> seen_user_ids;
}

// Main tray response
struct ReelsTrayResponse {
  1: list<TrayBucket> buckets;
  2: TrayPagingInfo paging_info;
  3: i32 total_buckets;
  4: i32 num_materialized;
  5: string request_id;
  6: bool has_self_story;
  7: i32 unseen_count;
  8: optional TrayBucket self_bucket;
}

// Ranking request for IGML pipelines (Shots/Brewery/Barkeep)
struct TrayRankingRequest {
  1: i64 viewer_id;
  2: list<i64> candidate_user_ids;
  3: i32 num_results;
  4: bool include_live;
  5: string ranking_model;
}

// Ranking response
struct TrayRankingResponse {
  1: list<i64> ranked_user_ids;
  2: list<double> ranking_scores;
  3: string request_id;
  4: string model_version;
}

// User metadata batch request (NodeAPI/LazyUserDict pattern)
struct UserMetadataBatchRequest {
  1: i64 viewer_id;
  2: list<i64> user_ids;
  3: bool include_story_info;
  4: bool include_live_info;
}

// User metadata batch response
struct UserMetadataBatchResponse {
  1: map<i64, TrayUserMetadata> user_metadata;
  2: string request_id;
  3: i32 total_fetched;
}

// Request for bucket clips (partial materialization)
struct TrayBucketClipsRequest {
  1: i64 viewer_id;
  2: i64 bucket_user_id;
  3: i32 num_items;
  4: optional string max_id;
}

// Response for bucket clips
struct TrayBucketClipsResponse {
  1: list<TrayReelItem> items;
  2: i32 total_items;
  3: bool more_available;
  4: string request_id;
}
