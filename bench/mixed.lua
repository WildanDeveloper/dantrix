-- wrk script for scenario S7: a more realistic load mixing file sizes.
--
-- wrk -t4 -c256 -d30s -s bench/mixed.lua http://127.0.0.1:8080/
--
-- The paths mirror the payload files that bench/make_files.sh creates.

math.randomseed(os.time())

local paths = {
    "/index.html",
    "/1k.bin",
    "/10k.bin",
    "/100k.bin",
    "/1m.bin",
}

-- Weighting: small files dominate a real static workload.
local weights = { 40, 25, 20, 10, 5 }

local total = 0
for _, w in ipairs(weights) do
    total = total + w
end

function request()
    local pick = math.random(total)
    local acc = 0
    for i, w in ipairs(weights) do
        acc = acc + w
        if pick <= acc then
            return wrk.format(nil, paths[i])
        end
    end
    return wrk.format(nil, "/index.html")
end
