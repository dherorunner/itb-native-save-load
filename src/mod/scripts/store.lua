-- Immutable P04 generations. Invalid/older indexes are never empty slots.
local Reader = __p04.reader
local M = {}
local PROTOCOL, COLLECTOR = 'p04-v2', 'p04-live-v2'
local INDEX, META, RECEIPT, LEDGER = 'p04-index-v2', 'p04-meta-v2', 'p04-receipt-v2', 'p04-ledger-v2'
local function clone(v)
    if type(v) ~= 'table' then return v end
    local out = {}; for k,x in pairs(v) do out[k] = clone(x) end; return out
end
local function str(v) return type(v) == 'string' and v ~= '' end
local function hash(v) return type(v) == 'string' and #v == 64 and v:match('^[%da-fA-F]+$') ~= nil end
local function component(v) return str(v) and #v <= 128 and v:match('^[%w_%-]+$') ~= nil end
local BIND = {'generation','kind','battle','profile','exe','epoch','seq','name','saved_at','protocol','collector'}
local function bound(a,b)
    if type(a) ~= 'table' or type(b) ~= 'table' then return false end
    for _,k in ipairs(BIND) do if a[k] == nil or a[k] ~= b[k] then return false end end
    return true
end
local function entry_ok(e)
    if type(e) ~= 'table' or e.protocol ~= PROTOCOL or e.collector ~= COLLECTOR then return false end
    for _,k in ipairs({'battle','exe','epoch','name','saved_at'}) do if not str(e[k]) then return false end end
    return component(e.generation) and component(e.profile) and (e.kind == 'auto' or e.kind == 'manual')
        and type(e.seq) == 'number' and e.seq > 0 and e.seq % 1 == 0
        and #e.name==12 and e.name:match('^p04_%x%x%x%x%.lua$') ~= nil and hash(e.s_sha) and hash(e.meta_sha)
end
function M.new(native, profile, epoch, clock)
    local S = {N=native, profile=profile, epoch=epoch, clock=clock or os.time}
    function S:dir() return 'profiles/profile_' .. self.profile end
    function S:index_path() return self:dir() .. '/index.sl' end
    function S:ledger_path() return 'ledger.sl' end
    function S:generation_dir(g) return self:dir() .. '/generations/' .. g end
    function S:load_index()
        if not component(self.profile) then return nil,'invalid','profile_invalid' end
        local exists,err = self.N.exists(self:index_path())
        if exists == nil then return nil,'unreadable',err or 'index_exists_failed' end
        if exists == false then return {schema=INDEX,protocol=PROTOCOL,profile=self.profile},'missing' end
        local text; text,err = self.N.read(self:index_path(),262144)
        if not text then return nil,'unreadable',err or 'index_read_failed' end
        local value; value,err = Reader.parse(text)
        if type(value) ~= 'table' or value.schema ~= INDEX or value.protocol ~= PROTOCOL or value.profile ~= self.profile then
            return nil,'corrupt',err or 'index_incompatible_or_corrupt'
        end
        for _,kind in ipairs({'auto','manual'}) do
            local e=value[kind]
            if e ~= nil and (not entry_ok(e) or e.kind ~= kind or e.profile ~= self.profile) then return nil,'corrupt',kind .. '_pointer_invalid' end
        end
        return value,'ok'
    end
    function S:read_generation(e)
        if not entry_ok(e) or e.profile ~= self.profile then return nil,'entry_invalid' end
        local base=self:generation_dir(e.generation)
        local texts={}
        for _,p in ipairs({{'meta','meta.sl',262144},{'live','live.sl',2097152},{'receipt','receipt.sl',262144},{'s','S.lua',8388608}}) do
            local exists,err=self.N.exists(base .. '/' .. p[2])
            if exists == nil then return nil,p[1] .. '_unreadable:' .. tostring(err) end
            if not exists then return nil,p[1] .. '_missing' end
            texts[p[1]],err=self.N.read(base .. '/' .. p[2],p[3])
            if not texts[p[1]] then return nil,p[1] .. '_unreadable:' .. tostring(err) end
        end
        if self.N.sha256(texts.meta) ~= e.meta_sha then return nil,'meta_hash_mismatch' end
        local m,err=Reader.parse(texts.meta)
        if type(m) ~= 'table' or m.schema ~= META or not bound(m,e) or type(m.shas) ~= 'table' or type(m.capture) ~= 'table' then
            return nil,err or 'meta_identity_mismatch'
        end
        for _,key in ipairs({'s','live','receipt'}) do
            if not hash(m.shas[key]) or self.N.sha256(texts[key]) ~= m.shas[key] then return nil,'generation_hash_mismatch:' .. key end
        end
        if e.s_sha ~= m.shas.s then return nil,'entry_hash_mismatch' end
        local live,le=Reader.parse(texts.live);local receipt,re=Reader.parse(texts.receipt)
        if type(live) ~= 'table' or live.schema ~= COLLECTOR or not bound(live,e) or live.s_sha ~= e.s_sha or type(live.snapshot) ~= 'table' then
            return nil,le or 'live_binding_invalid'
        end
        if type(receipt) ~= 'table' or receipt.schema ~= RECEIPT or not bound(receipt,e)
            or receipt.s_sha ~= e.s_sha or receipt.size ~= #texts.s or receipt.al ~= -1 or receipt.count ~= 1
            or receipt.write_al ~= 1 or receipt.capture_sha ~= e.s_sha or receipt.capture_size ~= #texts.s
            or receipt.capture_path ~= m.capture.path or m.capture.sha ~= e.s_sha or m.capture.size ~= #texts.s then
            return nil,re or 'receipt_binding_invalid'
        end
        local hint={region_key=m.region_key,map_name=m.map_name,mission_key=m.mission_key,
            mission_id=m.mission_id,marker_battle=m.marker_battle}
        local source,se=Reader.parse_snapshot(texts.s,hint)
        if not source then return nil,'source_invalid:' .. tostring(se) end
        local snap=live.snapshot
        for _,k in ipairs({'mission_key','mission_id','marker_battle','width','height','turn','team_turn'}) do
            if snap[k] == nil or m[k] == nil or snap[k] ~= m[k] or snap[k] ~= source[k] then return nil,'source_live_identity:' .. k end
        end
        if m.description == 'capture' then
            -- Region/map are capture-derived save description. The live save table
            -- may have been missing or stale at capture time; that provenance is
            -- stored separately instead of forcing both sides to agree.
            for _,k in ipairs({'region_key','map_name'}) do
                if m[k] ~= source[k] then return nil,'source_description:' .. k end
            end
            if m.live_region_key ~= snap.region_key or m.live_map_name ~= snap.map_name then
                return nil,'live_description_binding'
            end
        else
            -- Generations written by the previous package keep their exact
            -- source/live/metadata agreement and remain readable.
            for _,k in ipairs({'region_key','map_name'}) do
                if snap[k] == nil or snap[k] ~= m[k] or snap[k] ~= source[k] then return nil,'source_live_identity:' .. k end
            end
        end
        if snap.marker_battle ~= e.battle then return nil,'live_battle_invalid' end
        if type(snap.pawns)~='table' or type(snap.terrain)~='table' or type(snap.availability)~='table'
            or snap.availability.pawn_list~='value' or type(snap.deployed)~='boolean'
            or type(snap.deployment)~='table' or type(snap.deployment.in_progress)~='boolean' then return nil,'live_required_fields' end
        for i=1,64 do if type(snap.terrain[i])~='number' or snap.terrain[i]~=source.terrain[i] then return nil,'source_live_terrain' end end
        for id,p in pairs(source.pawns) do
            local q=snap.pawns[id]
            if type(q)~='table' then return nil,'source_live_pawn_set' end
            for _,k in ipairs({'type','team','health','x','y'}) do if p[k]==nil or q[k]==nil or p[k]~=q[k] then return nil,'source_live_pawn:' .. id .. '.' .. k end end
        end
        for id in pairs(snap.pawns) do if source.pawns[id]==nil then return nil,'source_live_pawn_set' end end
        for id=0,2 do
            local p=snap.pawns[id];local q=source.pawns[id]
            if not p or not q or type(p.active)~='boolean' or q.active~=p.active then return nil,'source_live_mech_active' end
        end
        return {meta=m,live=snap,receipt=receipt,s_text=texts.s,source=source}
    end
    function S:describe(e,battle)
        if e == nil then return 'empty' end
        if not entry_ok(e) then return 'corrupt' end
        if e.profile ~= self.profile then return 'other_profile' end
        local value,err=self:read_generation(e);if not value then return 'corrupt',err end
        if e.exe ~= self.N.exe_hash() then return 'incompatible' end
        if e.epoch ~= self.epoch then return 'other_process' end
        if not battle or e.battle ~= battle then return 'other_battle' end
        return 'valid'
    end
    function S:begin_generation(kind,battle,snapshot,result,name,seq)
        if kind ~= 'auto' and kind ~= 'manual' then return nil,'kind_invalid' end
        if type(snapshot) ~= 'table' or type(snapshot.s_text) ~= 'string' or type(snapshot.live) ~= 'table' or type(result) ~= 'table' then
            return nil,'snapshot_invalid'
        end
        local sha=self.N.sha256(snapshot.s_text)
        if not hash(sha) or result.capture_sha ~= sha or result.capture_size ~= #snapshot.s_text
            or result.al ~= -1 or result.count ~= 1 or result.write_al ~= 1 or not str(result.capture_path) then return nil,'capture_binding_invalid' end
        local random,err=self.N.random_hex(8)
        if type(random)~='string' or #random~=16 or not random:match('^%x+$') then return nil,err or 'random_invalid' end
        local e={generation='g' .. tostring(self.clock()) .. '_' .. random,kind=kind,battle=battle,profile=self.profile,
            exe=self.N.exe_hash(),epoch=self.epoch,seq=seq,name=name,saved_at=os.date('!%Y-%m-%dT%H:%M:%SZ',self.clock()),
            protocol=PROTOCOL,collector=COLLECTOR,s_sha=sha}
        local live=clone(e);live.schema=COLLECTOR;live.snapshot=clone(snapshot.live)
        local receipt=clone(e);receipt.schema=RECEIPT;receipt.al=result.al;receipt.count=result.count
        receipt.write_al=result.write_al;receipt.size=#snapshot.s_text;receipt.capture_sha=result.capture_sha
        receipt.capture_size=result.capture_size;receipt.capture_path=result.capture_path
        local meta=clone(e);meta.schema=META;meta.capture={path=result.capture_path,sha=sha,size=#snapshot.s_text}
        for _,k in ipairs({'mission_key','mission_id','marker_battle','width','height','turn','team_turn'}) do
            meta[k]=snapshot.live[k];e[k]=snapshot.live[k]
        end
        local source=snapshot.source
        if type(source) == 'table' then
            meta.description='capture'
            meta.live_region_key=snapshot.live.region_key
            meta.live_map_name=snapshot.live.map_name
            local capture_live=type(snapshot.capture_live)=='table' and snapshot.capture_live or snapshot.live
            meta.capture_live_region_key=capture_live.region_key
            meta.capture_live_map_name=capture_live.map_name
            for _,k in ipairs({'region_key','map_name','battle_index'}) do
                meta[k]=source[k];e[k]=source[k]
            end
            if (capture_live.region_key ~= nil and source.region_key ~= nil and capture_live.region_key ~= source.region_key)
                or (capture_live.map_name ~= nil and source.map_name ~= nil and capture_live.map_name ~= source.map_name) then
                meta.description_conflict=true
            end
            if type(snapshot.description_notes) == 'table' and #snapshot.description_notes > 0 then
                meta.description_notes=snapshot.description_notes
            end
        else
            meta.description='live'
            for _,k in ipairs({'region_key','map_name','battle_index'}) do
                meta[k]=snapshot.live[k];e[k]=snapshot.live[k]
            end
        end
        local lt,le=Reader.encode(live);local rt,re=Reader.encode(receipt);if not lt or not rt then return nil,le or re end
        meta.shas={s=sha,live=self.N.sha256(lt),receipt=self.N.sha256(rt)}
        local mt,me=Reader.encode(meta);if not mt then return nil,me end
        e.meta_sha=self.N.sha256(mt);if not entry_ok(e) then return nil,'entry_invalid' end
        local base=self:generation_dir(e.generation)
        for _,p in ipairs({{'S.lua',snapshot.s_text},{'receipt.sl',rt},{'live.sl',lt},{'meta.sl',mt}}) do
            local ok,we=self.N.write_exclusive(base .. '/' .. p[1],p[2]);if not ok then return nil,p[1] .. '_write:' .. tostring(we) end
            local back,be=self.N.read(base .. '/' .. p[1],8388608);if back ~= p[2] then return nil,p[1] .. '_readback:' .. tostring(be) end
        end
        local check,ce=self:read_generation(e);if not check then return nil,ce end
        return e
    end
    function S:publish(kind,e)
        if not entry_ok(e) or e.kind ~= kind then return nil,'entry_invalid' end
        local generation,ge=self:read_generation(e);if not generation then return nil,ge end
        local index,state,ie=self:load_index();if not index then return nil,'index_' .. tostring(state) .. ':' .. tostring(ie) end
        local old=self.N.read(self:index_path(),262144)
        index[kind]=clone(e)
        local text,ee=Reader.encode(index);if not text then return nil,ee end
        local ok,we=self.N.write_atomic(self:index_path(),text)
        if not ok then
            if we=='replace_failed' and self.N.read(self:index_path(),262144)==old then return nil,'old_preserved:replace_failed' end
            return nil,'commit_uncertain:' .. tostring(we)
        end
        if self.N.read(self:index_path(),262144) ~= text then return nil,'commit_uncertain:index_readback' end
        return true
    end
    function S:reserve_name()
        local ledger={schema=LEDGER,protocol=PROTOCOL,names={}}
        local exists,ee=self.N.exists(self:ledger_path());if exists == nil then return nil,'ledger_unreadable:' .. tostring(ee) end
        if exists then
            local text,re=self.N.read(self:ledger_path(),8388608);if not text then return nil,'ledger_unreadable:' .. tostring(re) end
            local parsed,pe=Reader.parse(text)
            if type(parsed) ~= 'table' or parsed.schema ~= LEDGER or parsed.protocol ~= PROTOCOL or type(parsed.names) ~= 'table' then
                return nil,'ledger_incompatible_or_corrupt:' .. tostring(pe)
            end
            ledger=parsed
        end
        local used,count,last={},0,0
        for k,n in pairs(ledger.names) do
            if type(k) ~= 'number' or k < 1 or k % 1 ~= 0 or type(n) ~= 'string' or #n~=12 or not n:match('^p04_%x%x%x%x%.lua$') then return nil,'ledger_invalid' end
            if used[n] then return nil,'ledger_invalid' end
            used[n]=true;count=count+1;if k>last then last=k end
        end
        if count~=last then return nil,'ledger_invalid' end
        for _=1,8 do
            local random,re=self.N.random_hex(2)
            if type(random) ~= 'string' or #random~=4 or not random:match('^%x+$') then return nil,re or 'random_failed' end
            local name='p04_' .. random:upper() .. '.lua'
            if not used[name] then
                ledger.names[#ledger.names+1]=name
                local text,te=Reader.encode(ledger);if not text then return nil,te end
                local ok,we=self.N.write_atomic(self:ledger_path(),text)
                if not ok then return nil,'ledger_commit_uncertain:' .. tostring(we) end
                if self.N.read(self:ledger_path(),8388608) ~= text then return nil,'ledger_commit_uncertain:readback' end
                return name
            end
        end
        return nil,'name_exhausted'
    end
    return S
end
return M
