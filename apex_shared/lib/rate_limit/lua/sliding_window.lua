-- Sliding Window Counter for Redis.
-- Keys: KEYS[1] = current window key, KEYS[2] = previous window key
-- Args: ARGV[1] = limit, ARGV[2] = window_size_ms, ARGV[3] = now_ms
--
-- Returns: {allowed (0/1), estimated_count, retry_after_ms}
--
-- 키 형식 예시:
--   Per-User:     "rl:user:{user_id}:cur", "rl:user:{user_id}:prev"
--   Per-Endpoint: "rl:ep:{user_id}:{msg_id}:cur", "rl:ep:{user_id}:{msg_id}:prev"

local cur_key = KEYS[1]
local prev_key = KEYS[2]
local limit = tonumber(ARGV[1])
local window_ms = tonumber(ARGV[2])
local now_ms = tonumber(ARGV[3])

-- Get current and previous window counts
local cur_count = tonumber(redis.call('GET', cur_key) or '0')
local prev_count = tonumber(redis.call('GET', prev_key) or '0')

-- Check if we need to detect window boundary.
-- We store window start timestamp alongside the current key.
local meta_key = cur_key .. ':meta'
local window_start = tonumber(redis.call('GET', meta_key) or '0')

if window_start == 0 then
    -- First request: initialize window
    window_start = now_ms
    redis.call('SET', meta_key, tostring(now_ms))
    redis.call('PEXPIRE', meta_key, window_ms * 3)
end

local elapsed = now_ms - window_start

if elapsed >= window_ms then
    -- Window rotation needed
    local windows_passed = math.floor(elapsed / window_ms)
    if windows_passed == 1 then
        -- Rotate: current -> previous
        redis.call('SET', prev_key, tostring(cur_count))
        redis.call('PEXPIRE', prev_key, window_ms * 2)
        cur_count = 0
        redis.call('SET', cur_key, '0')
        window_start = window_start + window_ms
    else
        -- 2+ windows: everything stale
        redis.call('SET', prev_key, '0')
        redis.call('PEXPIRE', prev_key, window_ms * 2)
        prev_count = 0
        cur_count = 0
        redis.call('SET', cur_key, '0')
        window_start = now_ms
    end
    redis.call('SET', meta_key, tostring(window_start))
    redis.call('PEXPIRE', meta_key, window_ms * 3)
    elapsed = now_ms - window_start
end

-- Compute weighted estimate
local ratio = elapsed / window_ms
if ratio > 1.0 then ratio = 1.0 end
local estimate = prev_count * (1.0 - ratio) + cur_count

if estimate >= limit then
    -- Denied: compute retry_after_ms
    -- Time until previous window weight drops enough
    -- prev_count * (1 - (elapsed + retry) / window) + cur_count < limit
    -- Simplified: wait until current window ends
    local retry_after = math.max(0, window_ms - elapsed)
    return {0, math.floor(estimate), math.floor(retry_after)}
end

-- Allowed: increment current counter
cur_count = redis.call('INCR', cur_key)
redis.call('PEXPIRE', cur_key, window_ms * 2)

return {1, math.floor(estimate), 0}
