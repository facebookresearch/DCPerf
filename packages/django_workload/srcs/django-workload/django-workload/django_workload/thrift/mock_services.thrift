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
