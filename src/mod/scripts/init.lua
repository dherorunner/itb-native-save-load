-- Runtime entry. The loader calls init/load; all Save/Load work is queued.
local Controller, UI, Escape = __p04.controller, __p04.ui, __p04.escape_menu
local mod = { id='itb_sl', name='原生存读档 / Native Save & Load', version='0.4.7', modApiVersion='2.9.6',
    gameVersion='1.2.93', author='dherorunner' }
local state = { initialized=false, subscriptions={}, native=nil, controller=nil, ui=nil, profile=nil }

local function log(message)
    if LOG then LOG('itb_sl P04: '..tostring(message)) end
end
local function profile()
    local ok,value=pcall(function() return modApi:getCurrentProfile() end)
    if ok then return value end
end
local function mission()
    if not modApi or modApi:getGameState()~='Mission' then return nil end
    return GetCurrentMission and GetCurrentMission() or nil
end
local function mission_binding()
    -- RegionData.iBattleRegion is part of the saved table and can still describe
    -- the island or the previous mission until the game writes a new save. Match
    -- the live mission instance to GAME.Missions instead; this is the only live
    -- identity used by the controller for a new battle.
    local current=mission()
    if not current then return nil end
    local key
    if GAME and type(GAME.Missions)=='table' then
        for id,entry in pairs(GAME.Missions) do
            if entry==current and type(id)=='number' and id%1==0 and id>=0 then
                if key then return nil end
                key='Mission'..id
            end
        end
    end
    if key then return key,'object' end
    -- A native load can replace the GAME/mission tables (new instances of the
    -- same battle). Accept only a unique identical battle marker there; a new
    -- auto has no marker and can never use this fallback.
    local token=current.itb_sl_p03
    local battle=type(token)=='table' and token.battle_id or nil
    if type(battle)=='string' and battle~='' and GAME and type(GAME.Missions)=='table' then
        local found
        for id,entry in pairs(GAME.Missions) do
            if type(entry)=='table' and type(entry.itb_sl_p03)=='table'
                and entry.itb_sl_p03.battle_id==battle and type(id)=='number' and id%1==0 and id>=0 then
                if found then return nil end
                found=id
            end
        end
        if found then return 'Mission'..found,'marker' end
    end
    return nil
end
local function region_binding(mission_key)
    -- Save description only: the assigned region may be missing or stale until a
    -- normal save. Never used as a condition for a real deployment window.
    if not mission_key or type(RegionData)~='table' then return nil end
    local found,key
    for candidate,region in pairs(RegionData) do
        if type(candidate)=='string' and (candidate:match('^region%d+$') or candidate=='final_region')
            and type(region)=='table' and region.mission==mission_key then
            if found then return nil end
            found,key=region,candidate
        end
    end
    return found,key
end
local PAWN_GETTERS={
    type='GetType',team='GetTeam',health='GetHealth',active='IsActive',dead='IsDead',corpse='IsCorpse',
    fire='IsFire',acid='IsAcid',frozen='IsFrozen',boosted='IsBoosted',shield='IsShield',
    max_health='GetMaxHealth',
}
local TILE_GETTERS={
    pawn='IsPawnSpace',fire='IsFire',smoke='IsSmoke',acid='IsAcid',frozen='IsFrozen',
    pod='IsPod',spawning='IsSpawning',damaged='IsDamaged',dangerous='IsDangerous',
    dangerous_item='IsDangerousItem',custom_tile='GetCustomTile',
}
local function read(object,name,...)
    if not object or type(object[name])~='function' then return nil,'missing' end
    local ok,value=pcall(object[name],object,...)
    if not ok then return nil,'error' end
    return value,value==nil and 'missing' or 'value'
end
local function engine()
    local E={}
    function E.profile() return profile() end
    function E.state() return modApi:getGameState() end
    function E.mission_ref() return mission() end
    function E.mission() return mission()~=nil end
    function E.mission_key() return mission_binding() end
    function E.ready() return Board~=nil and Game~=nil and mission()~=nil and mission_binding()~=nil end
    function E.busy() return E.state()=='Mission' and read(Board,'IsBusy')~=false end
    function E.team() return read(Game,'GetTeamTurn') end
    function E.battle_marker() local m=mission(); return m and m.itb_sl_p03 end
    function E.set_marker(marker) local m=mission(); if m then m.itb_sl_p03=marker end end
    function E.region_key() local _,key=region_binding(mission_binding()); return key end
    function E.map_name()
        local r=region_binding(mission_binding())
        return r and r.player and r.player.map_data and r.player.map_data.name
    end
    function E.dialog()
        if sdlext.isConsoleOpen and sdlext.isConsoleOpen() then return true end
        if sdlext.getShadowSurfaceRect then
            local x=sdlext.getShadowSurfaceRect()
            if x~=nil then return true end
        end
        if not sdlext.dialogVisible() then return false end
        return not (state.ui and state.ui.busy_owned and state.ui:busy_is_top())
    end
    function E.snapshot()
        if not E.ready() then return nil end
        local m=mission()
        local mkey,binding=mission_binding()
        if not m or not mkey then return nil end
        local r,key=region_binding(mkey)
        local map_name=r and r.player and r.player.map_data and r.player.map_data.name
        local battle_index=RegionData and RegionData.iBattleRegion
        -- Live Getter identity is required. Region/map fields are save description:
        -- they may be missing or stale here and are replaced by the verified
        -- capture metadata later; their live values keep their own provenance.
        local s={mission_key=mkey,mission_id=m.ID,binding_source=binding,
            marker_battle=m.itb_sl_p03 and m.itb_sl_p03.battle_id,
            pawns={},terrain={},tiles={},availability={},deployment={states={}},
            description={region_key='live',map_name='live'}}
        local function put(target,key_name,tag,value,availability)
            target[key_name]=value
            s.availability[tag]=availability or (value==nil and 'missing' or 'value')
        end
        put(s,'region_key','region_key',key)
        put(s,'map_name','map_name',map_name)
        put(s,'battle_index','battle_index',battle_index)
        put(s,'mission_key','mission_key',mkey,'value')
        put(s,'mission_id','mission_id',m.ID,'value')
        local size,size_status=read(Board,'GetSize')
        put(s,'width','width',size and size.x,size_status)
        put(s,'height','height',size and size.y,size_status)
        put(s,'turn','turn',read(Game,'GetTurnCount'))
        put(s,'team_turn','team_turn',read(Game,'GetTeamTurn'))
        if s.width~=8 or s.height~=8 then return s end
        for y=0,7 do
            for x=0,7 do
                local i=y*8+x+1
                local p=Point(x,y)
                put(s.terrain,i,'terrain.'..i,read(Board,'GetTerrain',p))
                local tile={}
                for attr,getter in pairs(TILE_GETTERS) do
                    local value,availability=read(Board,getter,p)
                    if value~=nil and type(value)~=(attr=='custom_tile' and 'string' or 'boolean') then
                        value=nil; availability='error'
                    end
                    put(tile,attr,'tile.'..i..'.'..attr,value,availability)
                end
                s.tiles[i]=tile
            end
        end
        local list,list_status=read(Board,'GetPawns',TEAM_ANY)
        s.availability.pawn_list=list_status
        local ids,seen={},{}
        if list then
            local count=list.size and list:size() or #list
            for i=1,count do
                local id=list.size and list:index(i) or list[i]
                if type(id)=='number' and id%1==0 and not seen[id] then ids[#ids+1]=id; seen[id]=true end
            end
        end
        -- Undeployed mechs are not necessarily included by GetPawns(TEAM_ANY).
        for id=0,2 do if not seen[id] then ids[#ids+1]=id; seen[id]=true end end
        table.sort(ids)
        for _,id in ipairs(ids) do
            local pawn=read(Board,'GetPawn',id)
            if pawn then
                local out={}
                for field,getter in pairs(PAWN_GETTERS) do
                    put(out,field,'pawn.'..id..'.'..field,read(pawn,getter))
                end
                local point,availability=read(pawn,'GetSpace')
                put(out,'x','pawn.'..id..'.x',point and point.x,availability)
                put(out,'y','pawn.'..id..'.y',point and point.y,availability)
                s.pawns[id]=out
            else
                s.availability['pawn.'..id..'.present']='missing'
            end
        end
        local deployment=m.deployment
        if type(deployment)=='table' then
            s.deployment.in_progress=deployment.in_progress
            s.deployment.phase=deployment.phase
            local deployed=0
            for id=0,2 do
                local entry=deployment[id]
                s.deployment.states[id]=type(entry)=='table' and entry.state or nil
                if s.deployment.states[id]==2 then deployed=deployed+1 end
            end
            s.deployment.deployed=deployed
        end
        s.deployed=false
        for id=0,2 do
            local pawn=s.pawns[id]
            if not pawn or pawn.x==nil or pawn.y==nil then s.deployed=nil; break end
            if pawn.x>=0 and pawn.y>=0 then s.deployed=true end
        end
        return s
    end
    function E.deploy_window()
        local s=E.snapshot()
        if not s or s.width~=8 or s.height~=8 or s.region_key=='final_region'
            or s.mission_id=='Mission_Final' or s.deployment.in_progress~=true
            or s.deployment.phase~=0 or E.busy() then return false end
        for id=0,2 do
            local p=s.pawns[id]
            local d=s.deployment.states[id]
            if not p or p.x~=-1 or p.y~=-1 or type(d)~='number' or d==2 or d==3 or d==4 then return false end
        end
        return true
    end
    function E.manual_window()
        local m=mission()
        local s=E.snapshot()
        return m~=nil and type(m.deployment)=='table' and m.deployment.in_progress==false
            and s~=nil and s.width==8 and s.height==8 and s.deployed==true
            and s.region_key~='final_region' and s.mission_id~='Mission_Final'
    end
    function E.set_terrain(x,y,value) return Board:SetTerrain(Point(x,y),value) end
    function E.tile_clear(x,y)
        local s=E.snapshot()
        local t=s and s.tiles[y*8+x+1]
        if not t then return nil end
        for attr in pairs(TILE_GETTERS) do
            if attr=='custom_tile' then if t[attr]~='' then return t[attr]==nil and nil or false end
            elseif t[attr]~=false then return t[attr]==nil and nil or false end
        end
        return true
    end
    return E
end

local function resolve_native()
    local script_path=mod.scriptPath
    if not script_path then
        local ok,current=pcall(function() return modApi:getCurrentMod() end)
        if ok and current then script_path=current.scriptPath end
    end
    if not script_path then return nil,'mod_path_missing' end
    local path=script_path:gsub('scripts[/\\]?$','')..'native/itb_sl_p04.dll'
    local ok,entry,why=pcall(package.loadlib,path,'luaopen_itb_sl_p04')
    if not ok or not entry then return nil,'loadlib:'..tostring(why or entry) end
    local loaded,native=pcall(entry)
    if not loaded or type(native)~='table' then return nil,'entry:'..tostring(native) end
    if native.protocol~='p04-v2' or native.bridge_abi~=2 then return nil,'bridge_incompatible' end
    for _,name in ipairs({'init','available','epoch','exe_hash','root','set_guard','next_seq','permit','revoke',
        'read','write_exclusive','write_atomic','exists','sha256','random_hex','save','load','poll','capture','verify'}) do
        if type(native[name])~='function' then return nil,'bridge_incomplete' end
    end
    return native
end
local unavailable={}
function unavailable:status()
    local reason=tostring(state.native_reason or '')
    local message='存读档服务初始化失败；本次未开始任何存读档操作。'
    if reason=='no_profile' then message='请先选择游戏档案，再使用存读档。'
    elseif reason=='unsupported_exe' then message='当前游戏版本不受此 Mod 支持。'
    elseif reason:match('^loadlib:') or reason:match('^entry:') then message='原生服务加载失败，请检查 Mod 文件与依赖。'
    elseif reason=='ui_bridge_incompatible' or reason:match('^bridge_') then
        message='Mod 与原生服务版本不配套，请使用同一包的文件。'
    end
    return {state='unavailable',message=message,
        fault=state.native_reason,can_open=true,slots={auto={status='unavailable'},manual={status='unavailable'}}}
end
function unavailable:open_panel() return self:status() end

local function wire_ui(controller)
    local ui=UI.new({sdlext=sdlext,controller=controller})
    if controller==unavailable then return ui end
    ui.actions.save_manual=function()
        local ok,reason=controller:request_manual_save()
        if not ok then log(reason); return end
        if controller.state=='confirm' then
            local old=controller.index.manual
            ui:confirm('覆盖临时手动槽','将覆盖保存时间 '..tostring(old and old.saved_at or ''),
                function(response) controller:confirm(response) end)
        end
    end
    local function load(slot,label)
        local ok,reason=controller:request_load(slot)
        if not ok then log(reason); return end
        local old=controller.index[slot]
        ui:confirm(label,'保存时间 '..tostring(old and old.saved_at or '')..'\n当前未保存的进度将丢失。',
            function(response) controller:confirm(response) end)
    end
    ui.actions.load_manual=function() load('manual','读取临时手动槽') end
    ui.actions.load_auto=function() load('auto','读取战前自动档') end
    return ui
end
local function attach()
    local root=sdlext.getUiRoot and sdlext.getUiRoot()
    if state.ui and root then state.ui:attach(root) end
    if state.escape and root then state.escape:attach(root) end
end
local function show_unavailable(reason)
    state.native_reason=reason
    log(reason)
    if state.ui then state.ui:detach() end
    state.ui=wire_ui(unavailable)
    attach()
end
local function bootstrap()
    state.profile=profile()
    if not state.profile or state.profile=='' then show_unavailable('no_profile'); return end
    local native,why=state.native,nil
    if not native then native,why=resolve_native() end
    if not native then show_unavailable(why); return end
    state.native=native
    -- An old business-compatible DLL must not start auto captures while its
    -- only supported UI entry is unavailable. Keep only the read-only reason.
    if native.ui_bridge~='p04-escape-v1' or type(native.request_escape_close)~='function' then
        show_unavailable('ui_bridge_incompatible'); return
    end
    local ok,initialized,err=pcall(native.init,state.profile,native.exe_hash())
    if not ok or initialized~=true or native.available()~=true then
        show_unavailable(err or initialized or 'init_failed'); return
    end
    state.engine=state.engine or engine()
    local controller=Controller.new({native=native,engine=state.engine,profile=state.profile,epoch=native.epoch()})
    if state.ui then state.ui:detach() end
    state.controller=controller
    local guard_ok,guard_err=native.set_guard(function(seq,kind,battle,p)
        local valid,value=pcall(controller.guard,controller,seq,kind,battle,p)
        return valid and value==true
    end)
    if guard_ok~=true then show_unavailable(guard_err or 'guard_unavailable'); state.controller=nil; return end
    state.native_reason=nil
    state.ui=wire_ui(controller)
    attach()
end
local function subscribe(name,handler)
    if state.subscriptions[name] then return end
    local event=modApi.events[name]
    if event then state.subscriptions[name]=event:subscribe(handler) end
end
local function dialog_components()
    -- Only logged when the wait reason changes. The shadow surfaces are a game
    -- drawing signal (HUD/tooltips can set them), not proof of a closable dialog.
    local shadow='unavailable'
    if sdlext.getShadowSurfaceRect then shadow=tostring(sdlext.getShadowSurfaceRect()~=nil) end
    local visible=tostring(sdlext.dialogVisible and sdlext.dialogVisible() or false)
    local owned=tostring(state.ui and state.ui.busy_owned or false)
    local top='false'
    if state.ui and state.ui.busy_is_top then
        local ok,value=pcall(state.ui.busy_is_top,state.ui)
        if ok then top=tostring(value) end
    end
    return 'shadow='..shadow..' dialogVisible='..visible..' busy_owned='..owned..' busy_top='..top
end
local function update()
    if state.escape then state.escape:update() end
    if not state.controller then return end
    local ok,err=pcall(state.controller.tick,state.controller)
    if not ok then
        log(err)
        if state.controller.active then state.native.revoke(state.controller.active.seq) end
        state.controller.active=nil
        state.controller.pending=nil
        state.controller.state='fault'
        state.controller.fault=tostring(err)
        state.controller.message='操作遇到错误，现场状态未确认；本次进程停用存读档。'
    end
    -- Record a changed auto wait reason in the normal loader log. No console or
    -- separate diagnostic collector is needed when the real entry cannot save.
    local c=state.controller
    local wait_reason=c.auto_note
    if c.candidate and not c:battle_id() then wait_reason=c:safe_reason() or wait_reason end
    if wait_reason and wait_reason~=state.auto_wait_reason then
        local detail=wait_reason
        if wait_reason=='其他窗口打开中' then detail=wait_reason..' ['..dialog_components()..']' end
        log('auto等待: '..detail)
    end
    state.auto_wait_reason=wait_reason
    -- Stage/result changes are logged once, not per frame. This is enough to tell
    -- a returned-but-waiting capture from a native/permit failure on a real run.
    local poll_note=c.poll_note
    if poll_note and poll_note~=state.poll_note then log('原生状态: '..poll_note) end
    state.poll_note=poll_note
    local post_note=c.post_note
    if post_note and post_note~=state.post_note then log('保存后等待: '..post_note) end
    state.post_note=post_note
    if state.ui then state.ui:sync_busy(state.controller.active~=nil) end
end
function mod:init()
    if state.initialized then return end
    state.initialized=true
    state.escape=Escape.new({sdlext=sdlext,
        context=function() return {profile=profile(),
            epoch=state.controller and state.controller.epoch,
            game=Game,mission=mission(),state=modApi:getGameState()} end,
        native=function() return state.native end,
        ui=function() return state.ui end})
    state.escape:install()
    -- Keep one controller tick per rendered frame, including deployment and
    -- modal busy panes, so completion does not depend on Mission.BaseUpdate.
    subscribe('onFrameDrawStart',update)
    subscribe('onUiRootCreated',function(_,root)
        if state.ui then state.ui:attach(root) end
        state.escape:attach(root)
    end)
    subscribe('onEscapeMenuWindowShown',function(...) state.escape:shown(...) end)
    subscribe('onEscapeMenuWindowHidden',function(...) state.escape:hidden(...) end)
    subscribe('onPreLoadGame',function() if state.controller then state.controller:external_load() end end)
    subscribe('onProfileChanged',function(_,current)
        state.profile=current
        if state.controller then
            local ok,err=pcall(state.controller.profile_changed,state.controller,current)
            if not ok then show_unavailable(err); state.controller=nil end
            if state.ui then state.ui:sync_busy(false) end
        else bootstrap() end
    end)
    bootstrap()
end
function mod:load()
    if not state.initialized then mod:init() end
    attach()
end
mod._state=state
return mod
