-- Copyright (c) Meta Platforms, Inc. and affiliates.
--
-- This source code is licensed under the MIT license found in the
-- LICENSE file in the root directory of this source tree.

-- Module instantiation
-- Initialize the pseudo random number generator
-- Resource: http://lua-users.org/wiki/MathLibraryTutorial
math.randomseed(os.time())
math.random(); math.random(); math.random()

-- =============================================================================
-- ID Tables for tracking entity IDs from responses
-- These tables store IDs returned by various endpoints for use with /seen
-- =============================================================================

-- Tables to store entity IDs (thread-local)
feed_timeline_ids = {}
clip_ids = {}
reels_tray_ids = {}
inbox_ids = {}

-- Maximum number of IDs to store per category to prevent unbounded growth
MAX_IDS_PER_CATEGORY = 1000

-- Low watermark: only process responses for ID extraction when total IDs < this value
-- This reduces CPU overhead from JSON parsing during high load
ID_LOW_WATERMARK = 100

-- =============================================================================
-- JSON Parsing Utilities
-- Simple JSON parsing for extracting IDs from responses
-- =============================================================================

-- Extract a string value for a given key from JSON
-- This is a simple pattern-based extraction, not a full JSON parser
function extract_json_string(json_str, key)
  local pattern = '"' .. key .. '"%s*:%s*"([^"]*)"'
  return json_str:match(pattern)
end

-- Check if a string looks like a valid UUID (8-4-4-4-12 hex format)
-- UUID format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
function is_valid_uuid(str)
  if str == nil or #str ~= 36 then
    return false
  end
  -- UUID pattern: 8 hex chars, hyphen, 4 hex chars, hyphen, 4 hex chars, hyphen, 4 hex chars, hyphen, 12 hex chars
  local pattern = "^[0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F]%-[0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F]%-[0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F]%-[0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F]%-[0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F]$"
  return str:match(pattern) ~= nil
end

-- Check if a string looks like an invalid/ad ID (starts with ad_, advertiser_, etc.)
function is_invalid_id(str)
  if str == nil then
    return true
  end
  -- Filter out ad IDs and advertiser IDs
  if str:match("^ad_") or str:match("^advertiser_") or str:match("^ad_media_") then
    return true
  end
  return false
end

-- Extract all IDs from a JSON array of objects
-- Looks for "id" or "pk" or "thread_id" fields
-- For clips, only extracts valid UUIDs (filters out ad_* and advertiser_* IDs)
function extract_ids_from_items(json_str, items_key, id_key, require_uuid)
  local ids = {}
  -- Find the items array
  local items_pattern = '"' .. items_key .. '"%s*:%s*%['
  local items_start = json_str:find(items_pattern)
  if items_start then
    -- Extract IDs using pattern matching
    -- Match both "id": "uuid" and "id": uuid (with or without quotes)
    local id_pattern = '"' .. id_key .. '"%s*:%s*"?([%w%-_]+)"?'
    for id in json_str:gmatch(id_pattern) do
      -- Skip invalid IDs (ad_*, advertiser_*, etc.)
      if not is_invalid_id(id) then
        -- If UUID is required, validate the format
        if require_uuid then
          if is_valid_uuid(id) then
            ids[#ids + 1] = id
          end
        else
          -- For non-UUID IDs (like thread_id), just do basic length check
          if #id >= 8 then
            ids[#ids + 1] = id
          end
        end
      end
    end
  end
  return ids
end

-- =============================================================================
-- Helper Functions
-- =============================================================================

-- Shuffle array
-- Returns a randomly shuffled array
function shuffle(paths)
  local j, k
  local n = #paths

  for i = 1, n do
    j, k = math.random(n), math.random(n)
    paths[j], paths[k] = paths[k], paths[j]
  end

  return paths
end

function split_str(str)
  local res = {}
  for part in str:gmatch("%S+") do
    res[#res + 1] = part
  end
  return res
end

function extract_path(url)
  return url:gsub("https?://[%w.]+:?%d*", "")
end

-- Add IDs to a table with size limit
function add_ids_to_table(tbl, new_ids)
  for _, id in ipairs(new_ids) do
    if #tbl < MAX_IDS_PER_CATEGORY then
      tbl[#tbl + 1] = id
    else
      -- Replace a random existing ID to maintain diversity
      local replace_idx = math.random(#tbl)
      tbl[replace_idx] = id
    end
  end
end

-- Pop a random ID from a table (returns nil if empty)
function pop_random_id(tbl)
  if #tbl == 0 then
    return nil
  end
  local idx = math.random(#tbl)
  local id = tbl[idx]
  -- Remove by swapping with last element and removing last
  tbl[idx] = tbl[#tbl]
  tbl[#tbl] = nil
  return id
end

-- Get entity type name for seen endpoint
function get_entity_type(endpoint_path)
  if endpoint_path:find("feed_timeline") then
    return "feed_timeline"
  elseif endpoint_path:find("clips") then
    return "clip"
  elseif endpoint_path:find("reels_tray") or endpoint_path:find("bundle_tray") then
    return "bundle"
  elseif endpoint_path:find("inbox") then
    return "inbox"
  end
  return nil
end

-- =============================================================================
-- Request Loading
-- =============================================================================

-- Load URL paths from the file
-- Format each line: <url/path> [method] [body]
function load_request_objects_from_file(file)
  local data = {}
  local content

  -- Check if the file exists
  -- Resource: http://stackoverflow.com/a/4991602/325852
  local f = io.open(file,"r")
  if f ~= nil then
    local lines = f:lines()
    for line in lines do
      local fields = split_str(line)
      local req = {}
      -- we expect the first field (URL) exists
      if fields[1] ~= nil then
        local url = fields[1]
        local path, _ = extract_path(url)
        req["path"] = path
        req["body"] = ""
        req["headers"] = {}

        -- find method
        if fields[2] ~= nil then
          req["method"] = fields[2]
        else
          req["method"] = "GET"
        end -- method

        -- find body text or file
        if fields[3] ~= nil then
          -- if starts with "<", read body data from file
          if string.char(fields[3]:byte(1)) == "<" then
            local body_fn = fields[3]:sub(2, #fields[3])
            local g = io.open(body_fn, "r")
            if g ~= nil then
              req["body"] = g:read("*all")
              io.close(g)
            end -- g ~= nil
          else
            req["body"] = fields[3]
          end -- fields[3][0] == '<'
        end -- fields[3] ~= nil
      end -- fields[1] ~= nil
      data[#data + 1] = req
      -- print(string.format("Adding request: %s %s (%d)",
      --   req["method"], req["path"], #req["body"]))
    end -- for line in lines
  else
    -- Return the empty array
    print(string.format("Could not open %s", file))
    return {}
  end -- f ~= nil

  return shuffle(data)
end

-- =============================================================================
-- Seen Request Generation
-- =============================================================================

-- Track last generated /seen request parameters for logging
last_seen_request = {
  method = nil,
  entity_type = nil,
  entity_id = nil,
}

-- Generate a dynamic /seen request with a random entity from collected IDs
function generate_seen_request()
  -- Randomly choose GET or POST method
  local method = math.random(2) == 1 and "GET" or "POST"

  -- Build list of available entity types with IDs
  local available = {}
  if #feed_timeline_ids > 0 then
    available[#available + 1] = {type = "feed_timeline", ids = feed_timeline_ids}
  end
  if #clip_ids > 0 then
    available[#available + 1] = {type = "clip", ids = clip_ids}
  end
  if #reels_tray_ids > 0 then
    available[#available + 1] = {type = "bundle", ids = reels_tray_ids}
  end

  -- If no IDs available, return default /seen request (original behavior)
  if #available == 0 then
    -- Track that we're sending a request with no params
    last_seen_request.method = method
    last_seen_request.entity_type = nil
    last_seen_request.entity_id = nil
    return wrk.format(method, "/seen", {}, "")
  end

  -- Randomly select an entity type
  local selected = available[math.random(#available)]
  local entity_type = selected.type
  local entity_id = pop_random_id(selected.ids)

  if entity_id == nil then
    -- Fallback to original behavior if pop failed
    last_seen_request.method = method
    last_seen_request.entity_type = nil
    last_seen_request.entity_id = nil
    return wrk.format(method, "/seen", {}, "")
  end

  -- Track the request parameters for logging in response callback
  last_seen_request.method = method
  last_seen_request.entity_type = entity_type
  last_seen_request.entity_id = entity_id

  -- Build request with entity parameters
  if method == "GET" then
    -- GET request with query parameters
    local path = string.format("/seen?type=%s&id=%s", entity_type, entity_id)
    return wrk.format("GET", path, {}, "")
  else
    -- POST request with JSON body
    local body = string.format('{"type":"%s","id":"%s"}', entity_type, entity_id)
    local headers = {["Content-Type"] = "application/json"}
    return wrk.format("POST", "/seen", headers, body)
  end
end

-- =============================================================================
-- Response Processing
-- =============================================================================

-- Get total count of IDs across all tables
function get_total_id_count()
  return #feed_timeline_ids + #clip_ids + #reels_tray_ids + #inbox_ids
end

-- Check if we should process responses for ID extraction
-- Only extract IDs when total count is below the low watermark
function should_extract_ids()
  return get_total_id_count() < ID_LOW_WATERMARK
end

-- Process response and extract IDs based on endpoint type
function process_response_for_ids(path, body)
  if body == nil or #body == 0 then
    return
  end

  -- feed_timeline: items array with "id" field (UUIDs)
  if path:find("feed_timeline") then
    local ids = extract_ids_from_items(body, "items", "id", true)  -- require UUID
    add_ids_to_table(feed_timeline_ids, ids)
    -- print(string.format("Extracted %d feed_timeline IDs (total: %d)", #ids, #feed_timeline_ids))
    return
  end

  -- clips: items_with_ads array with "pk" field (UUIDs only, filter out ad_* and advertiser_*)
  if path:find("clips") then
    local ids = extract_ids_from_items(body, "items_with_ads", "pk", true)  -- require UUID to filter out ads
    -- Also try "items" key as fallback
    if #ids == 0 then
      ids = extract_ids_from_items(body, "items", "pk", true)  -- require UUID
    end
    add_ids_to_table(clip_ids, ids)
    -- print(string.format("Extracted %d clip IDs (total: %d)", #ids, #clip_ids))
    return
  end

  -- reels_tray/bundle_tray: tray array with "pk" field (UUIDs)
  if path:find("reels_tray") or path:find("bundle_tray") then
    local ids = extract_ids_from_items(body, "tray", "pk", true)  -- require UUID
    -- Also try "bundles" or "entries" keys
    if #ids == 0 then
      ids = extract_ids_from_items(body, "bundles", "id", true)  -- require UUID
    end
    if #ids == 0 then
      ids = extract_ids_from_items(body, "entries", "id", true)  -- require UUID
    end
    add_ids_to_table(reels_tray_ids, ids)
    -- print(string.format("Extracted %d reels_tray IDs (total: %d)", #ids, #reels_tray_ids))
    return
  end

  -- inbox: threads array with "thread_id" field (NOT UUIDs - format: thread_xxx_xxx_xxx)
  if path:find("inbox") then
    local ids = extract_ids_from_items(body, "threads", "thread_id", false)  -- don't require UUID
    add_ids_to_table(inbox_ids, ids)
    -- print(string.format("Extracted %d inbox IDs (total: %d)", #ids, #inbox_ids))
    return
  end
end

-- =============================================================================
-- wrk Callbacks
-- =============================================================================

urls_txt_path = "urls.txt"
requests = {}
num_threads = 0

setup = function(thread)
  num_threads = num_threads + 1
end

init = function(args)
  if #args >= 1 then
    urls_txt_path = args[1]
    print("using urls txt from " .. urls_txt_path)
  end
  -- Load URL requests from file
  requests = load_request_objects_from_file(urls_txt_path)

  -- Check if at least one path was found in the file
  if #requests <= 0 then
    print("multiplerequests: No requests found.")
    os.exit()
  end

  print("multiplerequests: Found " .. #requests .. " requests")
  print("multiplerequests: ID tracking enabled for feed_timeline, clips, reels_tray, inbox")
end

-- Initialize the requests array iterator
counter = 1

request = function()
  -- Get the next requests array element
  local request_object = requests[counter]

  -- Increment the counter
  counter = counter + 1

  -- If the counter is longer than the requests array length then reset it
  if counter > #requests then
    counter = 1
  end

  -- Check if this is a /seen request - if so, generate dynamic request
  if request_object.path == "/seen" then
    return generate_seen_request()
  end

  -- Return the request object with the current URL path
  return wrk.format(
    request_object.method,
    request_object.path,
    request_object.headers,
    request_object.body
  )
end

-- Response callback to process responses and extract IDs
response = function(status, headers, body)
  -- Get the current request details
  local request_object = requests[((counter - 2) % #requests) + 1]
  local path = request_object.path
  local method = request_object.method
  local body_size = #body

  -- Process successful responses to extract IDs (only when below low watermark)
  if status >= 200 and status < 300 and should_extract_ids() then
    process_response_for_ids(path, body)
  end

  -- Build params string for /seen requests
  local params_str = ""
  if path == "/seen" and last_seen_request.method ~= nil then
    method = last_seen_request.method  -- Use the actual method used
    if last_seen_request.entity_type ~= nil and last_seen_request.entity_id ~= nil then
      params_str = string.format(" [type=%s, id=%s]",
        last_seen_request.entity_type, last_seen_request.entity_id)
    else
      params_str = " [no params]"
    end
  end

  -- Print in simplified format: HTTP/1.1 STATUS BYTES bytes ==> METHOD PATH [params]
  print(string.format("HTTP/1.1 %d   %5d bytes ==> %s  %s%s",
    status, body_size, method, path, params_str))
end

done = function(summary, latency, requests)
  -- Calculate basic metrics
  local failed_reqs = summary["errors"]["connect"]
    + summary["errors"]["read"]
    + summary["errors"]["write"]
    + summary["errors"]["status"]
    + summary["errors"]["timeout"]
  local successful_reqs = summary["requests"] - failed_reqs

  -- Print ID table statistics
  print("")
  print("=== Entity ID Collection Statistics ===")
  print(string.format("  feed_timeline IDs collected: %d", #feed_timeline_ids))
  print(string.format("  clip IDs collected: %d", #clip_ids))
  print(string.format("  reels_tray/bundle IDs collected: %d", #reels_tray_ids))
  print(string.format("  inbox IDs collected: %d", #inbox_ids))
  print("")

  -- Print summary metrics
  print("=== Performance Summary ===")
  print(string.format("Transactions: %d hits", summary["requests"]))
  local avail = 100.0 * successful_reqs / summary["requests"]
  print(string.format("Availability: %.2f %%", avail))
  print(string.format("Elapsed time: %.2f secs", summary["duration"] / 1e6))
  local data_mb = summary["bytes"] / 1048576
  print(string.format("Data transferred: %.2f MB", data_mb))
  print(string.format("Response time: %.3f secs", latency.mean / 1e6))
  local tx_rate = summary["requests"] / summary["duration"] * 1e6
  print(string.format("Transaction rate: %.2f trans/sec", tx_rate))
  local throughput = summary["bytes"] / 1048576 / summary["duration"] * 1e6
  print(string.format("Throughput: %.2f MB/sec", throughput))
  print(string.format("Concurrency: %d", num_threads))
  print(string.format("Successful transactions: %d", successful_reqs))
  print(string.format("Failed transactions: %d", failed_reqs))
  print(string.format("Longest transaction: %.3f", latency.max / 1e6))
  print(string.format("Shortest transaction: %.3f", latency.min / 1e6))
  print(string.format("P50: %.3f", latency:percentile(50.0) / 1e6))
  print(string.format("P90: %.3f", latency:percentile(90.0) / 1e6))
  print(string.format("P95: %.3f", latency:percentile(95.0) / 1e6))
  print(string.format("P99: %.3f", latency:percentile(99.0) / 1e6))
end

-- end
