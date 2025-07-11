-- Copyright (c) Meta Platforms, Inc. and affiliates.
--
-- This source code is licensed under the MIT license found in the
-- LICENSE file in the root directory of this source tree.

-- Module instantiation
-- Initialize the pseudo random number generator
-- Resource: http://lua-users.org/wiki/MathLibraryTutorial
math.randomseed(os.time())
math.random(); math.random(); math.random()

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
        -- req["headers"] = {["Connection"]="keep-alive", ["Keep-Alive"]="timeout=60, max=1024"}

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
      -- print(string.format("Adding request: %s %s (%d)", req["method"], req["path"], #req["body"]))
    end -- for line in lines
  else
    -- Return the empty array
    print(string.format("Could not open %s", file))
    return {}
  end -- f ~= nil

  return shuffle(data)
end

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

  -- Return the request object with the current URL path
  return wrk.format(request_object.method, request_object.path, request_object.headers, request_object.body)
end

done = function(summary, latency, requests)
  print(string.format("Transactions: %d hits", summary["requests"]))
  local failed_reqs = summary["errors"]["connect"] + summary["errors"]["read"] + summary["errors"]["write"] + summary["errors"]["status"] + summary["errors"]["timeout"]
  local successful_reqs = summary["requests"] - failed_reqs
  print(string.format("Availability: %.2f %%", 100.0 * successful_reqs / summary["requests"]))
  print(string.format("Elapsed time: %.2f secs", summary["duration"] / 1e6))
  print(string.format("Data transferred: %.2f MB", summary["bytes"] / 1048576))
  print(string.format("Response time: %.3f secs", latency.mean / 1e6))
  print(string.format("Transaction rate: %.2f trans/sec", summary["requests"] / summary["duration"] * 1e6))
  print(string.format("Throughput: %.2f MB/sec", summary["bytes"] / 1048576 / summary["duration"] * 1e6))
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
