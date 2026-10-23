-- Native buttons copy their resource geometry before the menu is visible.
-- Keep these six resources compact for the Mod's lifetime, including Main_Menu;
-- only the added widget is gated by game state / visibility. No native actions
-- are replaced. See the v3 acceptance report for the static copy path.
local Escape={}
local rows={escape_continue=0,escape_options=2,escape_ach=3,escape_menu=4,escape_quit=5,escape_abandon=6}
local BUTTON_W,BUTTON_H=175,50
local FIRST_ROW_Y,PITCH=78,56
local ABANDON_EXTRA,LINE_GAP=18,12
local owner,installed=false,false
local function enabled(c,S)
    return c and c.profile and c.profile~='' and (c.state=='Mission' or c.state=='Map' or c.state=='Island')
        and not (S.isMapEditor and S.isMapEditor())
end
local function rect(row)
    local box=Boxes and Boxes.escape_box
    if not box then return nil end
    return {x=box.x+(box.w-BUTTON_W)/2,y=box.y+FIRST_ROW_Y+row*PITCH+(row==6 and ABANDON_EXTRA or 0),
        w=BUTTON_W,h=BUTTON_H}
end
function Escape.rect(row)
    return rect(row)
end
function Escape:install()
    if owner and owner~=self then owner:restore_layout() end
    owner=self
    if installed then return end
    installed=true
    local button,line=GetButton,GetLine
    GetButton=function(name,...)
        if owner and rows[name]~=nil then owner:sync_layout() end
        local result=button(name,...)
        local layout=owner and owner.layout
        if layout and rows[name]~=nil and not layout.requested[name] then
            layout.requested[name]=true
            local r=Escape.rect(rows[name])
            if LOG then LOG('itb_sl P04: Esc layout v5 resource '..name..' rect='
                ..r.x..','..r.y..','..r.w..','..r.h) end
        end
        return result
    end
    GetLine=function(name,...)
        if owner and name=='escape_abandon_line' then owner:sync_layout() end
        return line(name,...)
    end
    local initialize=InitializeImageList
    if initialize then
        InitializeImageList=function(...)
            if owner then owner:restore_layout() end
            initialize(...)
            if owner then owner:sync_layout() end
        end
    end
    self:sync_layout()
end
function Escape:restore_layout()
    local layout=self.layout
    if not layout then return end
    for name,entry in pairs(layout.buttons) do
        if layout.button_table[name]==entry.applied then layout.button_table[name]=entry.original end
    end
    if layout.line_table.escape_abandon_line==layout.line then
        layout.line_table.escape_abandon_line=layout.original_line
    end
    self.layout=nil
end
function Escape:sync_layout()
    local box=Boxes and Boxes.escape_box
    if not box or not Buttons or not Lines then self:restore_layout();return false end
    local layout=self.layout
    local same=layout and layout.button_table==Buttons and layout.line_table==Lines
        and layout.x==box.x and layout.y==box.y and layout.w==box.w and layout.h==box.h
        and Lines.escape_abandon_line==layout.line
    if same then
        for name,entry in pairs(layout.buttons) do
            if Buttons[name]~=entry.applied then same=false;break end
        end
    end
    if same then return true end
    self:restore_layout()
    for name in pairs(rows) do if not Buttons[name] then return false end end
    layout={button_table=Buttons,line_table=Lines,buttons={},requested={},
        x=box.x,y=box.y,w=box.w,h=box.h,original_line=Lines.escape_abandon_line}
    for name,row in pairs(rows) do
        local original=Buttons[name]
        local r=Escape.rect(row)
        local applied=Button(Rect2D(r.x,r.y,r.w,r.h),original.label,12)
        layout.buttons[name]={original=original,applied=applied}
        Buttons[name]=applied
    end
    local r=rect(6)
    layout.line={Point(box.x+75,r.y-LINE_GAP),Point(box.x+box.w-75,r.y-LINE_GAP)}
    Lines.escape_abandon_line=layout.line
    self.layout=layout
    return true
end
function Escape.new(d)
    return setmetatable({S=d.sdlext,context=d.context,native=d.native,ui=d.ui,
        clock=d.clock or os.time,phase='idle',root=nil}, {__index=Escape})
end
function Escape:visible()
    return self.S.isEscapeMenuWindowVisible and self.S.isEscapeMenuWindowVisible()==true
end
function Escape:blocked()
    if self.S.dialogVisible and self.S.dialogVisible() then return true end
    if self.S.isConsoleOpen and self.S.isConsoleOpen() then return true end
    for _,name in ipairs({'isOptionsWindowVisible','isAchievementsWindowVisible','isAbandonTimelineWindowVisible',
        'isLanguageSelectionWindowVisible','isHotkeyConfigurationWindowVisible','isProfileSelectionWindowVisible',
        'isCreateProfileConfirmationWindowVisible','isDeleteProfileConfirmationWindowVisible','isStatisticsWindowVisible',
        'isNewGameWindowVisible','isPodWindowVisible','isPerfectIslandWindowVisible','isStoreWindowVisible',
        'isHangarUiVisible','isSquadSelectionWindowVisible','isCustomizeSquadWindowVisible',
        'isPilotSelectionWindowVisible','isDifficultySettingsWindowVisible','isMechColorWindowVisible'}) do
        if self.S[name] and self.S[name]() then return true end
    end
    return false
end
local function descendant(node,parent)
    while node do if node==parent then return true end node=node.parent end
    return false
end
function Escape:detach_button()
    local b=self.button
    if not b then return end
    b.visible=false
    local root=self.root
    if root then
        for _,field in ipairs({'pressedchild','hoveredchild','draggedchild','focuschild'}) do
            if descendant(root[field],b) then
                local setter=({pressedchild='setPressedChild',hoveredchild='setHoveredChild',draggedchild='setDraggedChild'})[field]
                if setter and root[setter] then root[setter](root,nil) else root[field]=nil end
            end
        end
    end
    b:detach();self.button=nil
end
function Escape:attach(root)
    if self.root~=root then self:detach_button();self.root=root end
end
function Escape:shown() self:update() end
function Escape:hidden() self:detach_button() end
function Escape:record(reason)
    self.last_error=reason
    local ui=self.ui()
    if ui and ui.controller then ui.controller.message='Esc 存读档入口：'..reason end
end
function Escape:cancel(reason)
    self.phase='idle';self.intent=nil;self.hidden_frames=0
    self:record(reason)
end
function Escape:explain(reason)
    self:record(reason)
    self.S.showDialog(function(pane,quit)
        pane.dismissible=true
        local frame=self.S.buildButtonDialog('存档 / 读档',function(scroll)
            local font=deco.uifont.tooltipTextLarge
            local text=UiWrappedText(reason,font.font,font.set):widthpx(scroll.w):addTo(scroll)
            text.pixelWrap=true;text:rebuild()
        end,function(layout)
            self.S.buildButton('关闭','',function() quit();return true end):addTo(layout)
        end,{maxW=0.7*ScreenSizeX(),maxH=0.8*ScreenSizeY()})
        frame:pospx((pane.w-frame.w)/2,(pane.h-frame.h)/2):addTo(pane)
    end)
end
function Escape:click()
    if self.phase~='idle' or not self.button or not self.button.parent or self.button.visible==false then return true end
    local c=self.context()
    if not enabled(c,self.S) or not self:visible() or self:blocked() then return true end
    local ui,N=self.ui(),self.native()
    local model=ui and ui.controller and ui.controller.status and ui.controller:status()
    if model and model.state=='unavailable' then
        self:explain(model.message);return true
    end
    if not N or N.ui_bridge~='p04-escape-v1' or type(N.request_escape_close)~='function'
        or not N.available or N.available()~=true then
        self:explain('Esc 关闭能力不可用，请使用同一构建包的 Mod 与原生 DLL；本次未开始存读档。')
        return true
    end
    if not ui or not ui.controller or ui.controller.active or ui.busy_owned then return true end
    self.intent={profile=c.profile,epoch=c.epoch,game=c.game,mission=c.mission,state=c.state,started=self.clock()}
    self.phase='closing_escape';self.hidden_frames=0
    self:detach_button()
    local ok,result,why=pcall(N.request_escape_close)
    if not ok or result~=true then
        local message='关闭请求失败：'..tostring(why or result)..'；本次未开始存读档。'
        self:cancel(message)
        -- There is no global feedback label, and the business panel cannot open
        -- while Esc remains. Make this failure readable without queuing work.
        self:explain(message)
        return true
    end
    self.phase='wait_hidden'
    return true
end
function Escape:update()
    local c=self.context()
    local root=self.S.getUiRoot and self.S.getUiRoot()
    self:attach(root)
    local layout_ready=self:sync_layout()
    if self.phase~='idle' then
        local i=self.intent
        if not enabled(c,self.S) or c.profile~=i.profile or c.epoch~=i.epoch or c.game~=i.game
            or c.mission~=i.mission or c.state~=i.state then self:cancel('上下文已变化，已取消');return end
        if self.clock()-i.started>=3 then
            local message='等待 Esc 关闭超时；未开始存读档'
            self:cancel(message)
            if self:visible() and not self:blocked() then self:explain(message) end
            return
        end
        local shadow=self.S.getShadowSurfaceRect and self.S.getShadowSurfaceRect()
        if not self:visible() and not self:blocked() and shadow==nil then
            self.hidden_frames=self.hidden_frames+1
        else self.hidden_frames=0 end
        if self.hidden_frames>=2 then
            self.phase='idle';self.intent=nil
            local ui=self.ui()
            local ok,why=ui:open()
            if not ok then self:record('面板未打开：'..tostring(why)) end
        end
        return
    end
    if not root or not layout_ready or not enabled(c,self.S) or not self:visible()
        or self:blocked() then self:detach_button();return end
    local r=Escape.rect(1)
    if not r then self:detach_button();return end
    if not self.button then
        local b
        b=self.S.buildButton('存档 / 读档','临时手动槽与战前自动档',function()
            if self.button~=b then return true end
            return self:click()
        end)
        -- Loader Ui forwards down/up to the game unless the handler consumes them.
        local down,up=b.mousedown,b.mouseup
        b.mousedown=function(widget,...) if down then down(widget,...) end return true end
        b.mouseup=function(widget,...) if up then up(widget,...) end return true end
        b:addTo(root);self.button=b
    end
    local scale=GetUiScale and GetUiScale() or 1
    self.button.visible=true
    self.button:pospx(r.x*scale,r.y*scale):widthpx(r.w*scale):heightpx(r.h*scale)
end
return Escape
