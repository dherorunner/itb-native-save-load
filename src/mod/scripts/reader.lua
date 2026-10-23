-- P04 restricted data reader/writer. Never executes Lua: it only accepts a small
-- literal subset plus the two inert constructors used by the game snapshot.
local M = {}
local MAX_BYTES, MAX_TOKENS, MAX_DEPTH = 8 * 1024 * 1024, 200000, 64
local ROOTS = {GameData=true, RegionData=true, GAME=true, SquadData=true, Profile=true, Settings=true}
local function integer(value)
    return type(value) == 'number' and value == value and math.abs(value) ~= math.huge and value % 1 == 0
end

local function fail(message) return nil, message end

local function tokenize(text)
    local tokens, i, n = {}, 1, #text
    while i <= n do
        local c = text:sub(i, i)
        if c == '-' and text:sub(i + 1, i + 1) == '-' then
            local line = text:find('\n', i, true)
            i = line and line + 1 or n + 1
        elseif c:match('%s') then
            i = i + 1
        elseif c == '"' then
            local j, buffer = i + 1, {}
            while j <= n do
                local ch = text:sub(j, j)
                if ch == '"' then break end
                if ch == '\\' then
                    local esc = text:sub(j + 1, j + 1)
                    if esc == 'n' then buffer[#buffer + 1] = '\n'
                    elseif esc == 't' then buffer[#buffer + 1] = '\t'
                    elseif esc == 'r' then buffer[#buffer + 1] = '\r'
                    elseif esc == 'a' then buffer[#buffer + 1] = string.char(7)
                    elseif esc == 'b' then buffer[#buffer + 1] = string.char(8)
                    elseif esc == 'f' then buffer[#buffer + 1] = string.char(12)
                    elseif esc == 'v' then buffer[#buffer + 1] = string.char(11)
                    elseif esc == '\n' then buffer[#buffer + 1] = '\n'
                    elseif esc == '\r' then
                        buffer[#buffer + 1] = '\n'
                        if text:sub(j + 2, j + 2) == '\n' then j = j + 1 end
                    elseif esc == '"' or esc == '\\' then buffer[#buffer + 1] = esc
                    elseif esc:match('%d') then
                        local digits = text:sub(j + 1, j + 3):match('^(%d%d?%d?)')
                        local byte = tonumber(digits)
                        if byte > 255 then return fail('bad byte escape') end
                        buffer[#buffer + 1] = string.char(byte)
                        j = j + #digits - 1
                    else return fail('unsupported escape') end
                    j = j + 2
                else
                    if ch == '\n' or ch == '\r' then return fail('unescaped newline in string') end
                    buffer[#buffer + 1] = ch
                    j = j + 1
                end
            end
            if text:sub(j, j) ~= '"' then return fail('unterminated string') end
            tokens[#tokens + 1] = { kind = 'string', value = table.concat(buffer) }
            i = j + 1
        elseif c:match('%d') or ((c == '-' or c == '+') and text:sub(i + 1, i + 1):match('%d')) then
            local j = i + 1
            while j <= n and text:sub(j, j):match('[%d%.eE%+%-]') do j = j + 1 end
            local raw = text:sub(i, j - 1)
            local value = tonumber(raw)
            if not value or value ~= value or math.abs(value) == math.huge then return fail('bad number') end
            tokens[#tokens + 1] = { kind = 'number', value = value }
            i = j
        elseif c:match('[%a_]') then
            local j = i
            while j <= n and text:sub(j, j):match('[%w_]') do j = j + 1 end
            tokens[#tokens + 1] = { kind = 'name', value = text:sub(i, j - 1) }
            i = j
        elseif c:match('[%{%}%[%]%(%)%,;=]') then
            tokens[#tokens + 1] = { kind = c }
            i = i + 1
        else
            return fail('unsupported character ' .. c)
        end
        if #tokens > MAX_TOKENS then return fail('too many tokens') end
    end
    tokens[#tokens + 1] = { kind = 'eof' }
    return tokens
end

local function parser(tokens, document_mode)
    local pos, depth = 1, 0
    local function kind() return tokens[pos] and tokens[pos].kind or 'eof' end
    local value
    local function table_value()
        pos = pos + 1
        local result, index, keys = {}, 1, {}
        while true do
            if kind() == '}' then pos = pos + 1; return result end
            local key
            if kind() == 'name' and tokens[pos + 1] and tokens[pos + 1].kind == '=' then
                key = tokens[pos].value
                pos = pos + 2
            elseif kind() == '[' then
                pos = pos + 1
                key = value()
                if kind() ~= ']' then error('bad key') end
                pos = pos + 1
                if kind() ~= '=' then error('bad key') end
                pos = pos + 1
                if type(key) ~= 'string' and not integer(key) then error('bad key type') end
            else
                key = index
                index = index + 1
            end
            if not document_mode and keys[key] then error('duplicate key') end
            keys[key] = true
            result[key] = value()
            if kind() == ',' or kind() == ';' then pos = pos + 1
            elseif kind() ~= '}' then error('missing separator') end
        end
    end
    local function read_value()
        local token = tokens[pos]
        if not token then error('unexpected end') end
        if token.kind == '{' then return table_value() end
        if token.kind == 'string' or token.kind == 'number' then
            pos = pos + 1
            return token.value
        end
        if token.kind == 'name' then
            if token.value == 'true' then pos = pos + 1 return true end
            if token.value == 'false' then pos = pos + 1 return false end
            if token.value == 'nil' then pos = pos + 1 return nil end
            if token.value == 'Point' or token.value == 'CreateEffect' then
                pos = pos + 1
                if kind() ~= '(' then error('ctor without args') end
                pos = pos + 1
                local args = {}
                if kind() ~= ')' then
                    while true do
                        local argument = value()
                        if argument == nil then error('nil constructor argument') end
                        args[#args + 1] = argument
                        if kind() == ',' then pos = pos + 1 else break end
                    end
                end
                if kind() ~= ')' then error('ctor not closed') end
                pos = pos + 1
                return { __ctor = token.value, args = args }
            end
            error('unsupported identifier ' .. token.value)
        end
        error('unexpected token ' .. token.kind)
    end
    value = function()
        depth = depth + 1
        if depth > MAX_DEPTH then error('nesting limit') end
        local result = read_value()
        depth = depth - 1
        return result
    end
    if document_mode then
        local ok, doc = pcall(function()
            local out, roots = {}, {}
            while kind() ~= 'eof' do
                if kind() ~= 'name' then error('root name expected') end
                local key = tokens[pos].value
                pos = pos + 1
                if not ROOTS[key] or roots[key] then error('unsupported or duplicate root') end
                roots[key] = true
                if kind() ~= '=' then error('root = expected') end
                pos = pos + 1
                out[key] = value()
                if type(out[key]) ~= 'table' then error('root is not a table') end
            end
            return out
        end)
        if not ok then return fail(tostring(doc)) end
        return doc
    end
    local ok, result = pcall(value)
    if not ok then return fail(tostring(result)) end
    if kind() ~= 'eof' then return fail('trailing tokens') end
    return result
end

-- Parses either "return { ... }" (our .sl files) or a bare table/document body.
function M.parse(text)
    if type(text) ~= 'string' or #text == 0 then return fail('empty document') end
    if #text > MAX_BYTES then return fail('document too large') end
    local body = text:gsub('^%s*return%s+', '', 1)
    local tokens, token_error = tokenize(body)
    if not tokens then return fail(token_error) end
    local parsed, parse_error = parser(tokens, false)
    if parsed == nil then return fail(parse_error or 'parse failed') end
    if type(parsed) ~= 'table' then return fail('document root is not a table') end
    return parsed
end

local function point(value)
    if type(value) ~= 'table' or value.__ctor ~= 'Point' then return nil end
    local args = value.args or {}
    if #args ~= 2 or not integer(args[1]) or not integer(args[2]) then return nil end
    return { x = args[1], y = args[2] }
end

local function region_names(region)
    local names = {}
    for name in pairs(region) do
        if type(name) == 'string' and (name:match('^region%d+$') or name == 'final_region') then
            names[#names + 1] = name
        end
    end
    table.sort(names)
    return names
end

-- The live RegionData table may still describe the island or the previous
-- mission. A verified live battle binding (mission_key) selects the captured
-- region by its mission assignment; the capture's own iBattleRegion remains
-- auxiliary evidence and is recorded separately.
local function current_region(region, hint)
    local battle = region.iBattleRegion
    local source_key
    if integer(battle) and battle >= 0 then
        source_key = battle == 20 and 'final_region' or ('region' .. tostring(battle))
    end
    local wanted = type(hint) == 'table' and hint.mission_key or nil
    local found, found_key
    if wanted ~= nil then
        for _, name in ipairs(region_names(region)) do
            local entry = region[name]
            if type(entry) == 'table' and entry.mission == wanted then
                if found then return nil, nil, '任务区域分配不唯一' end
                found, found_key = entry, name
            end
        end
        if not found then return nil, nil, '缺少当前任务的区域' end
        return found, found_key, nil, source_key
    end
    if source_key then
        local entry = region[source_key]
        if type(entry) == 'table' then return entry, source_key, nil, source_key end
    end
    if type(hint) == 'table' and type(hint.region_key) == 'string' and type(region[hint.region_key]) == 'table' then
        return region[hint.region_key], hint.region_key, nil, source_key
    end
    return nil, nil, '缺少合法 iBattleRegion'
end

local function snapshot_of(doc, hint)
    local region = doc.RegionData
    if type(region) ~= 'table' then return fail('缺少 RegionData') end
    local current, key, region_error, source_key = current_region(region, hint)
    if not current then return fail(region_error or '缺少当前 region') end
    local player = current.player
    if type(player) ~= 'table' or type(player.map_data) ~= 'table' then return fail('缺少 player.map_data') end
    local board = player.map_data
    local dims = point(board.dimensions)
    if not dims or dims.x < 1 or dims.y < 1 or dims.x > 64 or dims.y > 64 then return fail('棋盘尺寸非法') end
    local map_name
    if board.name ~= nil then
        if type(board.name) ~= 'string' then return fail('地图名称非法') end
        if board.name ~= '' then map_name = board.name end
    end
    if type(board.map) ~= 'table' then return fail('缺少地图记录') end
    local records, plain_forest = {}, {}
    for _, record in pairs(board.map) do
        if type(record) ~= 'table' then return fail('地图记录不是表') end
        local loc = point(record.loc)
        local terrain = record.terrain
        if not loc or not integer(terrain) or terrain < 0 or terrain > 2147483647
            or loc.x < 0 or loc.x > dims.x - 1 or loc.y < 0 or loc.y > dims.y - 1 then
            return fail('地图记录非法')
        end
        if records[loc.x .. ',' .. loc.y] ~= nil then return fail('地图坐标重复') end
        records[loc.x .. ',' .. loc.y] = record
        local plain = terrain == 6
        for name in pairs(record) do if name ~= 'loc' and name ~= 'terrain' then plain = false end end
        plain_forest[loc.x .. ',' .. loc.y] = plain
    end
    local terrain_grid, checksum = {}, 0
    for y = 0, dims.y - 1 do
        for x = 0, dims.x - 1 do
            local record = records[x .. ',' .. y]
            local cell = record and record.terrain or 0
            terrain_grid[#terrain_grid + 1] = cell
            checksum = (checksum * 31 + cell + x * 7 + y * 11) % 2147483647
        end
    end
    local pawns, pawn_count = {}, 0
    for name, pawn in pairs(board) do
        if type(name) == 'string' and name:match('^pawn%d+$') then
            if type(pawn) ~= 'table' then return fail('单位记录不是表') end
            local id = pawn.id
            local loc = point(pawn.location)
            if not integer(id) or id < 0 or id > 2147483647 then return fail('单位缺少合法 id') end
            if not loc or not ((loc.x == -1 and loc.y == -1)
                or (loc.x >= 0 and loc.y >= 0 and loc.x < dims.x and loc.y < dims.y)) then
                return fail('单位缺少合法位置')
            end
            if type(pawn.type) ~= 'string' or pawn.type == '' or not integer(pawn.iTeamId)
                or not integer(pawn.health) then return fail('单位缺少类型、队伍或血量') end
            if pawn.bActive ~= nil and type(pawn.bActive) ~= 'boolean' then return fail('单位 active 非法') end
            if pawns[id] then return fail('单位 id 重复') end
            pawn_count = pawn_count + 1
            pawns[id] = {
                type = pawn.type, team = pawn.iTeamId, health = pawn.health,
                max_health = pawn.max_health, active = pawn.bActive,
                -- is_corpse is the persistent Corpse flag (IsCorpse), not IsDead.
                -- A healthy mech/train can have it set. Do not infer death from it.
                corpse = pawn.is_corpse, x = loc.x, y = loc.y,
            }
        end
    end
    if board.pawn_count ~= nil and (not integer(board.pawn_count) or board.pawn_count ~= pawn_count) then
        return fail('单位数量不符')
    end
    local mission_key = current.mission
    local binding = type(hint) == 'table' and hint or {}
    if mission_key == nil then
        -- The selected region was bound by the live mission; a capture that did
        -- not repeat the mission key still carries the mission object below.
        mission_key = binding.mission_key
    elseif binding.mission_key ~= nil and mission_key ~= binding.mission_key then
        return fail('源任务索引与实时战斗不符')
    end
    if type(mission_key) ~= 'string' and not integer(mission_key) then return fail('缺少 mission key') end
    if not integer(player.iCurrentTurn) or not integer(player.iTeamTurn) then return fail('缺少回合字段') end
    local game = doc.GAME or {}
    if type(game) ~= 'table' then return fail('GAME 不是表') end
    local missions = game.Missions
    local mission_id, marker
    if type(missions) == 'table' and mission_key ~= nil then
        local numeric = type(mission_key) == 'number' and mission_key or tonumber(mission_key:match('^Mission(%d+)$'))
        local mission = missions[mission_key] or (numeric and missions[numeric])
        if type(mission) == 'table' then
            mission_id = mission.ID
            marker = mission.itb_sl_p03
        end
    end
    if type(mission_id) ~= 'string' or mission_id == '' then return fail('缺少当前 mission ID') end
    if binding.mission_id ~= nil and mission_id ~= binding.mission_id then return fail('源任务ID与实时战斗不符') end
    local marker_battle = type(marker) == 'table' and marker.battle_id or nil
    if binding.marker_battle ~= nil and marker_battle ~= binding.marker_battle then
        return fail('源战斗标识与实时战斗不符')
    end
    local battle_index
    if source_key == 'final_region' then battle_index = 20
    elseif type(source_key) == 'string' then battle_index = tonumber(source_key:match('^region(%d+)$')) end
    return {
        region_key = key, battle_index = battle_index,
        source_region_key = source_key, mission_key = mission_key,
        mission_id = mission_id, map_name = map_name,
        width = dims.x, height = dims.y, terrain = terrain_grid, records = records, plain_forest = plain_forest,
        pawns = pawns, turn = player.iCurrentTurn, team_turn = player.iTeamTurn,
        player_state = player.iState, sMission = player.sMission, marker = marker,
        marker_battle = marker_battle,
        checksum = checksum,
    }
end

function M.project(doc, hint)
    if type(doc) ~= 'table' then return fail('document root is not a table') end
    local snap, err = snapshot_of(doc, hint)
    if not snap then return fail(err) end
    return snap
end

function M.parse_document(text)
    if type(text) ~= 'string' or #text == 0 then return fail('empty document') end
    if #text > MAX_BYTES then return fail('document too large') end
    local tokens, token_error = tokenize(text)
    if not tokens then return fail(token_error) end
    return parser(tokens, true)
end

function M.parse_snapshot(text, hint)
    local doc, err = M.parse_document(text)
    if not doc then return fail(err) end
    return M.project(doc, hint)
end

local function encode_value(value, indent, seen)
    if indent >= MAX_DEPTH then return nil, 'nesting limit' end
    local t = type(value)
    if t == 'nil' then return 'nil' end
    if t == 'boolean' then return tostring(value) end
    if t == 'number' then
        if value ~= value or math.abs(value) == math.huge then return nil, 'non-finite number' end
        if value % 1 == 0 and math.abs(value) < 1e15 then return string.format('%.0f', value) end
        return string.format('%.17g', value)
    end
    if t == 'string' then
        return '"' .. value:gsub('[%z\1-\31\\"]', function(ch)
            if ch == '\\' or ch == '"' then return '\\' .. ch end
            return string.format('\\%03d', ch:byte())
        end) .. '"'
    end
    if t ~= 'table' then return nil, 'unsupported type ' .. t end
    if seen[value] then return nil, 'cyclic table' end
    seen[value] = true
    local pad = string.rep('  ', indent)
    local inner = string.rep('  ', indent + 1)
    local parts, count = {}, 0
    for i = 1, #value do count = count + 1 end
    local is_array = count > 0
    for key in pairs(value) do
        if type(key) ~= 'number' or key % 1 ~= 0 or key < 1 or key > count then is_array = false end
    end
    if is_array then
        for i = 1, count do
            local text, err = encode_value(value[i], indent + 1, seen)
            if not text then return nil, err end
            parts[#parts + 1] = inner .. text
        end
    else
        local keys = {}
        for key in pairs(value) do
            if type(key) ~= 'string' and not integer(key) then return nil, 'invalid key' end
            keys[#keys + 1] = key
        end
        table.sort(keys, function(a,b) return type(a) == type(b) and a < b or type(a) < type(b) end)
        for _, key in ipairs(keys) do
            local text, err = encode_value(value[key], indent + 1, seen)
            if not text then return nil, err end
            local encoded_key = encode_value(key, indent + 1, seen)
            parts[#parts + 1] = inner .. '[' .. encoded_key .. '] = ' .. text
        end
    end
    seen[value] = nil
    if #parts == 0 then return '{}' end
    return '{\n' .. table.concat(parts, ',\n') .. '\n' .. pad .. '}'
end

function M.encode(value)
    if type(value) ~= 'table' then return fail('document root is not a table') end
    local text, err = encode_value(value, 0, {})
    if not text then return fail(err) end
    if #text + 8 > MAX_BYTES then return fail('document too large') end
    return 'return ' .. text .. '\n'
end

return M
