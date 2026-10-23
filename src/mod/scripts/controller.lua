-- Serial P04 business state machine. Native return, verification and publication are separate gates.
local Reader, Store = __p04.reader, __p04.store
local C = {}
local TTL, TIMEOUT = 8000, 8
local IDENTITY={'mission_id','mission_key','marker_battle','width','height','turn','team_turn'}
local DESCRIPTION={'region_key','map_name'}
local BASE={'type','team','health','x','y'}
local EXTRA={'active','dead','corpse','fire','acid','frozen','boosted','shield','max_health'}
local TILE={'pawn','fire','smoke','acid','frozen','pod','spawning','damaged','dangerous','dangerous_item','custom_tile'}
local function copy(v)
    if type(v) ~= 'table' then return v end
    local o={};for k,x in pairs(v) do o[k]=copy(x) end;return o
end
local function encode(v) return Reader.encode(v) end
-- Signature/identity view of a live snapshot. Region/map fields are save
-- description and may be missing or refreshed by an engine save; they never
-- decide whether the real board/mission changed.
local function core(s)
    if type(s) ~= 'table' then return s end
    local availability={}
    for k,v in pairs(type(s.availability)=='table' and s.availability or {}) do
        if k~='region_key' and k~='map_name' and k~='battle_index' then availability[k]=v end
    end
    return {mission_key=s.mission_key,mission_id=s.mission_id,marker_battle=s.marker_battle,
        width=s.width,height=s.height,turn=s.turn,team_turn=s.team_turn,deployed=s.deployed,
        pawns=copy(s.pawns),terrain=copy(s.terrain),tiles=copy(s.tiles),deployment=copy(s.deployment),
        availability=availability}
end
local function freeze_copy(f)
    return {profile=f.profile,epoch=f.epoch,battle=f.battle,ref=f.ref,signature=f.signature,
        window=f.window,snapshot=copy(f.snapshot)}
end
local function integer(v) return type(v)=='number' and v%1==0 and math.abs(v)<2147483648 end
local function valid(s,marker)
    if type(s) ~= 'table' or s.width ~= 8 or s.height ~= 8 or type(s.pawns) ~= 'table' or type(s.terrain) ~= 'table' then return false,'棋盘或单位不可读' end
    for _,k in ipairs({'mission_key','mission_id'}) do
        if type(s[k])~='string' or s[k]=='' then return false,'身份字段不可读 ' .. k end
    end
    if not integer(s.turn) or s.turn<0 or not integer(s.team_turn) then return false,'回合字段不可读' end
    if type(s.availability)~='table' or s.availability.pawn_list~='value' then return false,'单位集合来源不可读' end
    if marker and s.marker_battle ~= marker then return false,'战斗标识不符' end
    for i=1,64 do if not integer(s.terrain[i]) or s.terrain[i]<0 then return false,'缺少地形 ' .. i end end
    for id,p in pairs(s.pawns) do
        if not integer(id) or id<0 or type(p) ~= 'table' then return false,'单位集合不可读' end
        for _,k in ipairs(BASE) do if p[k] == nil then return false,'单位缺字段 ' .. id .. '.' .. k end end
        if type(p.type)~='string' or p.type=='' or not integer(p.team) or not integer(p.health) or p.health<0
            or not integer(p.x) or not integer(p.y) or not ((p.x==-1 and p.y==-1) or (p.x>=0 and p.x<=7 and p.y>=0 and p.y<=7)) then return false,'单位字段非法 ' .. id end
        for _,k in ipairs(EXTRA) do
            if p[k]~=nil and ((k=='max_health' and (not integer(p[k]) or p[k]<0)) or (k~='max_health' and type(p[k])~='boolean')) then return false,'单位属性非法 ' .. id .. '.' .. k end
        end
    end
    for id=0,2 do if type(s.pawns[id]) ~= 'table' or type(s.pawns[id].active) ~= 'boolean' then return false,'机甲状态不可读 ' .. id end end
    if type(s.deployment) ~= 'table' or type(s.deployed) ~= 'boolean' or type(s.deployment.in_progress)~='boolean' then return false,'部署状态不可读' end
    return true
end
local function compare(expected,actual,source,ignore_terrain)
    local diffs,terrain,notes={},{},{}
    if type(expected) ~= 'table' or type(actual) ~= 'table' then return {'状态不可读'},terrain,notes end
    for _,k in ipairs(IDENTITY) do
        if expected[k] == nil or actual[k] == nil or expected[k] ~= actual[k] then diffs[#diffs+1]='身份 ' .. k end
    end
    for _,k in ipairs(DESCRIPTION) do
        if expected[k] ~= nil and actual[k] ~= nil and expected[k] ~= actual[k] then
            notes[#notes+1]='描述 ' .. k .. ' ' .. tostring(expected[k]) .. ' -> ' .. tostring(actual[k])
        end
    end
    for i=1,64 do
        local a=type(expected.terrain)=='table' and expected.terrain[i]
        local b=type(actual.terrain)=='table' and actual.terrain[i]
        if a == nil or b == nil or a ~= b then terrain[#terrain+1]={index=i,target=a,actual=b} end
    end
    if not ignore_terrain and #terrain>0 then diffs[#diffs+1]='地形不符' end
    local ep,ap=expected.pawns or {},actual.pawns or {}
    for id,p in pairs(ep) do
        local q=ap[id]
        if type(q) ~= 'table' then diffs[#diffs+1]='单位缺失 ' .. id
        else
            for _,k in ipairs(BASE) do if p[k] == nil or q[k] == nil or p[k] ~= q[k] then diffs[#diffs+1]='单位 ' .. id .. '.' .. k end end
            for _,k in ipairs(EXTRA) do
                if not source or (p[k] ~= nil and q[k] ~= nil) or (k=='active' and id>=0 and id<=2) then
                    if (k=='active' and id>=0 and id<=2 and (p[k]==nil or q[k]==nil)) or p[k] ~= q[k] then
                        diffs[#diffs+1]='单位 ' .. id .. '.' .. k
                    end
                end
            end
        end
    end
    for id in pairs(ap) do if ep[id] == nil then diffs[#diffs+1]='新增单位 ' .. id end end
    if not source then
        if expected.deployed == nil or actual.deployed == nil or expected.deployed ~= actual.deployed then diffs[#diffs+1]='部署不符' end
        if encode(expected.deployment) ~= encode(actual.deployment) then diffs[#diffs+1]='部署字段不符' end
        -- Compare readable optional tile/getter values; both-missing remains a coverage gap.
        for i=1,64 do
            local a=type(expected.tiles)=='table' and expected.tiles[i] or {}
            local b=type(actual.tiles)=='table' and actual.tiles[i] or {}
            for _,k in ipairs(TILE) do if a[k] ~= b[k] then diffs[#diffs+1]='格状态 ' .. i .. '.' .. k end end
        end
    end
    return diffs,terrain,notes
end
function C.new(deps)
    local self=setmetatable({N=deps.native,E=deps.engine,profile=deps.profile,clock=deps.clock or os.time,
        epoch=deps.epoch or deps.native.epoch(),state='idle',message='',index={},auto_tries={},battle_counter=0,
        stable_count=0,outside_count=0,saved_count=0,load_counts=0,setter_counts=0,auto_captures=0}, {__index=C})
    self.store=Store.new(self.N,self.profile,self.epoch,self.clock)
    local index,state,err=self.store:load_index()
    if index then self.index=index else self:fail('槽目录不可用: ' .. tostring(state) .. ':' .. tostring(err),true) end
    return self
end
function C:battle_marker() return self.E.battle_marker() end
function C:battle_id()
    local m=self:battle_marker();return type(m)=='table' and m.battle_id or nil
end
function C:safe_reason(dialog)
    if self.fault then return 'SL已停用: ' .. self.fault end
    if not self.N or self.N.protocol ~= 'p04-v2' or self.N.available() ~= true then return '原生服务不可用' end
    if self.E.profile and self.E.profile() ~= self.profile then return 'profile已变化' end
    if self.N.epoch() ~= self.epoch then return '游戏进程已变化' end
    if self.E.ready() ~= true or self.E.state() ~= 'Mission' or self.E.mission() ~= true then return '不在有效战斗' end
    if self.E.busy() ~= false then return '局面正在变化' end
    -- Before deployment the game uses team 6 although no enemy action is running.
    -- Admit only the independently checked all-undeployed window; ordinary enemy
    -- turns and a partially deployed board still cannot queue Save/Load.
    local team=self.E.team()
    if team ~= 1 and not (team == 6 and self.E.deploy_window() == true) then return '不是玩家回合' end
    if dialog ~= false and self.E.dialog and self.E.dialog() then return '其他窗口打开中' end
    return nil
end
function C:window()
    if self.E.deploy_window() == true then return 'deploy' end
    if self.E.manual_window() == true then return 'manual' end
end
function C:frozen()
    local s=self.E.snapshot();local ok=valid(s)
    if not ok then return nil end
    return {profile=self.profile,epoch=self.epoch,battle=self:battle_id(),ref=self.E.mission_ref(),
        signature=encode(core(s)),window=self:window(),snapshot=copy(s)}
end
function C:matches(f,dialog)
    if not f or self:safe_reason(dialog) or f.profile ~= self.profile or f.epoch ~= self.epoch
        or f.ref ~= self.E.mission_ref() or f.battle ~= self:battle_id() or f.window ~= self:window() then return false end
    local live=self.E.snapshot()
    return type(live)=='table' and f.signature == encode(core(live))
end
-- Identity-only gate for the returned/capture/completion phase. A transient
-- game window or a save-table refresh after the capture must not strand a real
-- verified capture in the 8 s wait; the board/source comparison below still
-- rejects any real state change. External loads change the mission instance and
-- are caught here for our own Save.
function C:post_identity_reason(a)
    if self.fault then return 'SL已停用: ' .. self.fault end
    if not self.N or self.N.protocol ~= 'p04-v2' or self.N.available() ~= true then return '原生服务不可用' end
    if self.E.profile and self.E.profile() ~= self.profile then return 'profile已变化' end
    if self.N.epoch() ~= self.epoch then return '游戏进程已变化' end
    if self.E.state() ~= 'Mission' or self.E.mission() ~= true then return '战斗已结束或离开任务' end
    if a.kind == 'save' and self.E.mission_ref() ~= a.frozen.ref then return '战斗对象已变化' end
    local marker=self.E.battle_marker()
    if type(marker) ~= 'table' or marker.battle_id ~= a.battle then return '战斗标识已变化' end
    return nil
end
function C:source_identity(source,frozen,a)
    if type(source) ~= 'table' then return false,'源不可读' end
    for _,k in ipairs({'mission_id','mission_key','turn','team_turn','width','height'}) do
        if frozen[k] == nil or source[k] ~= frozen[k] then return false,'源 ' .. k .. ' 与捕获前不符' end
    end
    if source.marker_battle ~= a.battle then return false,'源战斗标识不符' end
    return true
end
function C:slot_views()
    local out={}
    for _,kind in ipairs({'auto','manual'}) do
        local e=self.index[kind]
        local status,reason=self.store:describe(e,self:battle_id())
        out[kind]={status=status,reason=reason,kind=kind,label=e and e.mission_id,battle=e and e.battle,
            saved_at=e and e.saved_at,generation=e and e.generation,same_battle=e and e.battle==self:battle_id()}
    end
    return out
end
function C:status()
    local slots=self:slot_views()
    local readable=valid(self.E.snapshot())
    local ready=self.state=='idle' and not self.pending and readable and not self:safe_reason(false) and self.stable_count>=2
    local gaps=0;local s=self.E.snapshot()
    if type(s)=='table' and type(s.availability)=='table' then for _,v in pairs(s.availability) do if v~='value' then gaps=gaps+1 end end end
    return {state=self.state,message=self.message,fault=self.fault,profile=self.profile,epoch=self.epoch,slots=slots,coverage_missing=gaps,
        can_open=true,auto_note=self.auto_note,action_allowed={save_manual=ready and self:window()=='manual' or false,
        load_manual=ready and self:window()~=nil and slots.manual.status=='valid' or false,
        load_auto=ready and self:window()~=nil and slots.auto.status=='valid' or false}}
end
function C:open_panel()
    self.panel=self:frozen()
    return self:status()
end
function C:ensure_marker(adopted)
    local m=self:battle_marker();if m then return m end
    self.battle_counter=self.battle_counter+1
    m={battle_id=self.epoch .. '_' .. self.battle_counter,adopted=adopted==true}
    self.E.set_marker(m)
    if self:battle_id() ~= m.battle_id then return nil end
    return m
end
function C:request_manual_save()
    if self.state~='idle' or self.pending then return false,'忙碌' end
    if not self:matches(self.panel) or self.stable_count<2 or self:window()~='manual' then return false,'保存窗口已变化' end
    if not self:ensure_marker(true) then return false,'无法标识战斗' end
    self.panel=self:frozen()
    self.pending={action='save',slot='manual',frozen=freeze_copy(self.panel),confirmed=self.index.manual==nil,
        confirm_label=self.index.manual and self.index.manual.saved_at}
    self.state=self.pending.confirmed and 'pending' or 'confirm'
    return true
end
function C:request_load(slot)
    if slot~='manual' and slot~='auto' then return false,'无效槽位' end
    if self.state~='idle' or self.pending then return false,'忙碌' end
    if not self:matches(self.panel) or self.stable_count<2 or not self:window() then return false,'读取窗口已变化' end
    local e=self.index[slot];local status,reason=self.store:describe(e,self:battle_id())
    if status~='valid' then return false,reason or status end
    self.pending={action='load',slot=slot,entry=copy(e),frozen=freeze_copy(self.panel),confirmed=false}
    self.state='confirm';return true
end
function C:confirm(response)
    if not self.pending or self.state~='confirm' then return false end
    if response~=1 then self:cancel_pending('已取消');return false end
    if not self:matches(self.pending.frozen,false) then self:cancel_pending('局面已变化，未发送');return false end
    self.pending.confirmed=true;self.state='pending';return true
end
function C:cancel_pending(reason)
    self.pending=nil;self.panel=nil;self.state='idle';self.message=reason or '已取消'
end
function C:fail(message,terminal)
    if self.active then
        self.N.revoke(self.active.seq)
        if self.active.slot=='auto' and self.active.kind=='save' then self.auto_tries[self.active.battle]='failed' end
    end
    self.active=nil;self.pending=nil;self.panel=nil;self.message=message
    self.state=terminal and 'fault' or 'idle';if terminal then self.fault=message;self.suppressed=true end
end
function C:guard(seq,kind,battle,profile)
    local a=self.active
    return a~=nil and a.seq==seq and a.kind==kind and a.battle==battle and a.profile==profile
        and a.epoch==self.epoch and self.clock()-a.started<TIMEOUT and a.phase=='queued'
        and self:matches(a.frozen) == true
end
function C:send(kind,slot,frozen,entry,attempt)
    local seq,err=self.N.next_seq();if not seq then self:fail('未开始: ' .. tostring(err));return end
    local a={seq=seq,kind=kind,slot=slot,frozen=frozen,battle=frozen.battle,profile=self.profile,
        epoch=self.epoch,exe=self.N.exe_hash(),started=self.clock(),phase='queued',entry=entry,attempt=attempt or 1}
    if kind=='load' then
        local g,ge=self.store:read_generation(entry)
        if not g then self:fail('槽不可用: ' .. tostring(ge));return end
        if self.store:describe(entry,a.battle)~='valid' then self:fail('槽不属于当前战斗');return end
        a.generation=g;self.suppressed=true
    end
    self.active=a
    local permitted,pe=self.N.permit(seq,kind,a.battle,TTL)
    if not permitted then self:fail('未开始: ' .. tostring(pe));return end
    for _=1,8 do
        local name,ne=self.store:reserve_name();if not name then self:fail('未开始: ' .. tostring(ne));return end
        a.name=name
        local ok,se
        if kind=='save' then ok,se=self.N.save(seq,name,slot)
        else
            local base=self.N.root() .. '/' .. self.store:generation_dir(entry.generation)
            ok,se=self.N.load(seq,name,base .. '/S.lua',entry.s_sha,base .. '/receipt.sl',a.generation.meta.shas.receipt)
        end
        if ok then self.state=kind=='save' and (slot=='auto' and 'saving_auto' or 'saving') or 'loading';self.message=kind=='save' and '正在保存' or '正在读取';return end
        -- These synchronous errors guarantee no native call. A called/unknown action is never retried.
        if se~='name_occupied' and se~='name_collision' then self:fail('未开始: ' .. tostring(se));return end
    end
    self:fail('未开始: 传输名不足')
end
function C:result_matches(r,a)
    return type(r)=='table' and r.seq==a.seq and r.kind==a.kind and r.epoch==a.epoch
        and r.name==a.name and r.profile==a.profile and r.exe==a.exe
end
function C:try_repair(expected,live,terrain,a)
    if a.slot~='auto' or a.repair_attempted or #terrain~=1 then return false,'不允许地形修补' end
    local cell=terrain[1]
    if cell.actual~=0 or cell.target~=6 or self.E.deploy_window()~=true or live.deployed~=false or expected.deployed~=false
        or a.generation.meta.battle~=self:battle_id() or self:battle_id()~=expected.marker_battle then return false,'修补窗口或来源不符' end
    local x=(cell.index-1)%8;local y=math.floor((cell.index-1)/8)
    if a.generation.source.plain_forest[x .. ',' .. y]~=true then return false,'源不是普通森林' end
    for _,s in ipairs({expected,live}) do
        local tile=type(s.tiles)=='table' and s.tiles[cell.index]
        if type(tile)~='table' then return false,'缺少目标格状态' end
        for _,k in ipairs(TILE) do
            if (k=='custom_tile' and tile[k]~='') or (k~='custom_tile' and tile[k]~=false) then return false,'目标格状态不清空: ' .. k end
        end
        for _,p in pairs(s.pawns) do if p.x==x and p.y==y then return false,'目标格有单位' end end
    end
    if self.E.tile_clear(x,y)~=true then return false,'目标格不可核对' end
    a.repair_attempted=true;self.setter_counts=self.setter_counts+1
    local ok,result=pcall(self.E.set_terrain,x,y,6)
    if not ok or result==false then return false,'地形修补调用失败' end
    return true
end
function C:complete(a)
    local live=self.E.snapshot()
    local readable,why=valid(live,a.battle)
    local identity=self:post_identity_reason(a)
    local diffs=readable and compare(a.kind=='save' and a.frozen.snapshot or a.generation.live,live,false,false) or {'状态不可读: ' .. tostring(why)}
    if identity or #diffs>0 then
        self:fail('最终局面核验失败，SL已停用: ' .. table.concat(diffs,'、') .. tostring(identity or ''),true);return
    end
    if a.kind=='save' then
        local ok,err=self.store:publish(a.slot,a.new_entry)
        if not ok then
            if tostring(err):find('old_preserved:',1,true)==1 then self:fail('保存失败且旧槽保留: ' .. tostring(err));return end
            self:fail('提交状态不确定: ' .. tostring(err),true);return
        end
        self.index[a.slot]=a.new_entry;self.saved_count=self.saved_count+1
        if a.slot=='auto' then self.auto_tries[a.battle]='ok';self.auto_captures=self.auto_captures+1;self.auto_note='本场战前档已生成' end
        self.message=a.slot=='auto' and '战前档已保存' or '已保存'
    else self.load_counts=self.load_counts+1;self.message='已读取，可继续操作' end
    self.N.revoke(a.seq);self.active=nil;self.state='idle';self.panel=nil
end
function C:post_check(a)
    local identity=self:post_identity_reason(a)
    if identity then
        self:fail((a.kind=='load' and '读档失败且现场状态未确认: ' or '保存失败且旧槽保留: ') .. identity,true);return
    end
    local live=self.E.snapshot();local ok,why=valid(live,a.battle)
    if not ok then self:fail((a.kind=='load' and '读档失败且现场状态未确认: ' or '保存失败且旧槽保留: ') .. tostring(why),true);return end
    local sig=encode(core(live))
    if sig==a.post_sig then a.post_count=(a.post_count or 0)+1 else a.post_sig=sig;a.post_count=1 end
    if a.post_count<2 then
        -- First/only change goes to the normal loader log; no per-frame spam.
        self.post_note='等待稳定的保存后局面 样本=' .. tostring(a.post_count)
        return
    end
    self.post_note=nil
    if a.kind=='save' then
        local ds,_,notes=compare(a.frozen.snapshot,live,false,false)
        local source_diff,_,source_notes=compare(a.source,live,true,false)
        if #ds>0 or #source_diff>0 then
            self:fail('保存失败且旧槽保留: ' .. table.concat(ds,'、') .. table.concat(source_diff,'、'),true);return
        end
        a.description_notes={}
        for _,note in ipairs(notes) do a.description_notes[#a.description_notes+1]=note end
        for _,note in ipairs(source_notes) do a.description_notes[#a.description_notes+1]=note end
        local e,err=self.store:begin_generation(a.slot,a.battle,
            {s_text=a.capture,live=live,source=a.source,capture_live=a.frozen.snapshot,
             description_notes=a.description_notes},a.result,a.name,a.seq)
        if not e then self:fail('保存失败且旧槽保留: ' .. tostring(err),true);return end
        a.new_entry=e
    else
        local ds,terrain=compare(a.generation.live,live,false,true)
        if #ds>0 then self:fail('读档失败且现场状态未确认: ' .. table.concat(ds,'、'),true);return end
        if #terrain>0 then
            local fixed,err=self:try_repair(a.generation.live,live,terrain,a)
            if not fixed then self:fail('读档失败且现场状态未确认: ' .. tostring(err),true);return end
            a.post_count=0;a.post_sig=nil;a.phase='post';return
        end
    end
    local verified,err=self.N.verify(a.seq,a.kind)
    if not verified then self:fail('原生核验提交失败: ' .. tostring(err),true);return end
    a.phase='verifying'
end
function C:poll_active()
    local a=self.active
    local r=self.N.poll()
    local note='poll state=' .. tostring(r and r.state) .. ' native_called=' .. tostring(r and r.native_called)
        .. ' al=' .. tostring(r and r.al) .. ' count=' .. tostring(r and r.count)
        .. ' write_al=' .. tostring(r and r.write_al) .. ' capture=' .. tostring(r and r.capture_size)
    if note ~= self.poll_note then self.poll_note=note end
    if self.clock()-a.started>=TIMEOUT then
        self.N.revoke(a.seq)
        local known=self:result_matches(r,a) and r.native_called==false
        self:fail(known and '未开始: 操作超时' or '操作超时，结果不明；SL已停用',not known);return
    end
    local returned=self:result_matches(r,a) and (r.state=='returned' or r.state=='verified')
    local running=self:result_matches(r,a) and r.native_called==true
    if a.phase=='queued' and not returned and not running and not a.called and not self:matches(a.frozen) then
        self.N.revoke(a.seq);a.invalid=true
        if self:result_matches(r,a) and (r.native_called==true or r.state=='returned' or r.state=='verified') then
            self:fail('窗口已变化，操作结果未确认',true);return
        end
    end
    if self:result_matches(r,a) then
        if r.native_called==true then a.called=true end
        if r.state=='rejected' or r.state=='failed' then
            if r.state=='rejected' and r.native_called==false and r.reason=='name_occupied' and a.attempt<8 and self:matches(a.frozen) then
                self.N.revoke(a.seq);self.active=nil
                self:send(a.kind,a.slot,a.frozen,a.entry,a.attempt+1);return
            end
            local prefix=r.native_called==false and '未开始: ' or (a.kind=='save' and '保存失败且旧槽保留: ' or '读档失败且现场状态未确认: ')
            self:fail(prefix .. tostring(r.reason),r.native_called~=false);return
        end
        if a.invalid and r.native_called==false then self:fail('局面已变化，未发送');return end
        if a.phase=='queued' and (r.state=='returned' or r.state=='verified') then
            if r.native_called~=true or (a.kind=='save' and (r.al~=-1 or r.count~=1 or r.write_al~=1))
                or (a.kind=='load' and (r.al~=1 or r.prepare_ok~=true)) then
                self:fail(a.kind=='save' and '保存失败且旧槽保留: 原生结果未确认' or '读档失败且现场状态未确认: 原生结果未确认',true);return
            end
            a.result=copy(r);a.phase='post';a.post_count=0
            if a.kind=='save' then
                local capture,err=self.N.capture(a.seq)
                if type(capture)~='string' or capture=='' or r.capture_size~=#capture or self.N.sha256(capture)~=r.capture_sha then
                    self:fail('保存失败且旧槽保留: 捕获绑定不符 ' .. tostring(err),true);return
                end
                local source,se=Reader.parse_snapshot(capture,{mission_key=a.frozen.snapshot.mission_key,
                    mission_id=a.frozen.snapshot.mission_id,marker_battle=a.battle,
                    region_key=a.frozen.snapshot.region_key})
                if not source then self:fail('保存失败且旧槽保留: 源解析失败 ' .. tostring(se),true);return end
                local identity_ok,identity_err=self:source_identity(source,a.frozen.snapshot,a)
                if not identity_ok then self:fail('保存失败且旧槽保留: ' .. tostring(identity_err),true);return end
                a.capture=capture;a.source=source;self.post_note='已取得源，等待稳定局面'
            end
        elseif a.phase=='verifying' and r.state=='verified' then self:complete(a);return end
    end
    if self.active and a.phase=='post' then self:post_check(a) end
end
function C:tick()
    if self.state=='fault' then return end
    local in_mission=self.E.state()=='Mission' and self.E.mission()==true
    if not in_mission then
        self.stable_count=0;self.last_signature=nil
        -- Native shadow surfaces also occur on the normal island map. Observe
        -- the mission-free map here; safe_reason still guards actual Save/Load.
        local outside=(self.E.state()=='Map' or self.E.state()=='Island') and self.E.mission()==false
            and self.E.busy()==false
        if outside and (not self.E.profile or self.E.profile()==self.profile) then self.outside_count=self.outside_count+1 else self.outside_count=0 end
        if self.outside_count>=2 then self.new_entry=true;self.suppressed=false;self.candidate=nil end
    else
        self.outside_count=0
        local snap=self.E.snapshot();local signature=snap and encode(core(snap))
        if signature and signature==self.last_signature then self.stable_count=self.stable_count+1 else self.last_signature=signature;self.stable_count=1 end
        if self.new_entry then
            self.new_entry=false
            if not self:battle_id() and not self.suppressed then self.candidate=self.E.mission_ref() else self.candidate=nil end
        end
    end
    if self.active then self:poll_active();return end
    if self.pending then
        if not self:matches(self.pending.frozen,false) then self:cancel_pending('局面已变化，未发送');return end
        if not self.pending.confirmed then return end
        if self.E.dialog and self.E.dialog() then return end
        local p=self.pending;self.pending=nil;self:send(p.action=='save' and 'save' or 'load',p.slot,p.frozen,p.entry);return
    end
    if not in_mission or self:safe_reason() or self.stable_count<2 then return end
    if self.candidate and self.candidate~=self.E.mission_ref() then self.candidate=nil end
    if not self.candidate or self:battle_id() or self.suppressed then
        if not self:battle_id() then self.auto_note='本场未捕获战前档；下一场新战斗可生成' end
        return
    end
    if self.E.deploy_window()~=true then
        if self.E.manual_window()==true then self.candidate=nil end
        self.auto_note='等待稳定的战前部署窗口';return
    end
    local ok=valid(self.E.snapshot());if not ok then self.auto_note='战前状态不可读';return end
    self.candidate=nil
    local m=self:ensure_marker(false);if not m then self.auto_note='无法标识新战斗';return end
    if self.auto_tries[m.battle_id] then return end
    self.auto_tries[m.battle_id]='trying';self:send('save','auto',self:frozen())
end
function C:external_load()
    self.new_entry=false;self.candidate=nil;self.suppressed=true;self.stable_count=0;self.last_signature=nil
    if self.active and self.active.kind=='load' then
        local result=self.N.poll()
        if self:result_matches(result,self.active) and result.native_called==true then return end
    end
    if self.active then self:fail('外部读档中，操作结果未确认',true) else self:cancel_pending('外部读档，已取消') end
end
function C:profile_changed(profile)
    local uncertain=self.active~=nil
    if self.active then self.N.revoke(self.active.seq) end
    self.active=nil;self.pending=nil;self.panel=nil;self.suppressed=true;self.new_entry=false;self.candidate=nil
    self.profile=profile;self.state='idle';self.fault=nil;self.stable_count=0;self.last_signature=nil
    local ok,err=self.N.init(profile,self.N.exe_hash())
    if not ok then self:fail('profile服务初始化失败: ' .. tostring(err),true);return end
    self.epoch=self.N.epoch();self.store=Store.new(self.N,profile,self.epoch,self.clock)
    local index,state,ie=self.store:load_index()
    if not index then self.index={};self:fail('槽目录不可用: ' .. tostring(state) .. ':' .. tostring(ie),true);return end
    self.index=index
    if uncertain then self:fail('profile切换时操作结果未确认；SL已停用',true) end
end
C.compare=compare
return C
