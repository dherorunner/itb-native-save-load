-- Two-slot panel and an input-consuming non-pausing busy pane. Entry belongs to Esc.
local UI={}
local STATUS_TEXT={empty='空',valid='可用',other_battle='其他战斗',other_process='本次进程外',
    corrupt='损坏',other_profile='其他档案',unavailable='不可用'}
local function slot_text(slot)
    if not slot or slot.status=='empty' then return '空' end
    local status=STATUS_TEXT[slot.status] or '不可用'
    return status..(slot.label and (' · '..slot.label) or '')..(slot.saved_at and (' · '..slot.saved_at) or '')
end
function UI.confirm_accepted(response) return response==1 end
function UI.can_read(slot) return slot and slot.status=='valid' end
function UI.panel_text(model)
    local lines={'战前自动档：'..slot_text(model.slots.auto),'临时手动槽：'..slot_text(model.slots.manual)}
    if model.auto_note then lines[#lines+1]=model.auto_note end
    if model.message and model.message~='' then lines[#lines+1]='状态：'..model.message end
    if model.state=='fault' then lines[#lines+1]='本次进程的存读档已停用，请保留现场。' end
    return table.concat(lines,'\n')
end
function UI.new(deps)
    return setmetatable({sdlext=deps.sdlext,controller=deps.controller,actions=deps.actions or {}},
        {__index=UI})
end
function UI:busy_is_top()
    local root=self.sdlext.getUiRoot()
    local focused=root and root.focuschild
    while focused do
        if focused==self.busy_pane then return true end
        focused=focused.parent
    end
    return false
end
function UI:sync_busy(busy)
    if busy and not self.busy_owned then
        if self.sdlext.dialogVisible() then return end
        self.busy_owned=true
        self.sdlext.showDialog(function(pane,quit)
            pane.dismissible=false
            self.busy_pane=pane
            self.busy_quit=quit
            pane.onDialogExit=function()
                self.busy_owned=false;self.busy_quit=nil;self.busy_pane=nil
            end
            -- The loader background pane consumes mouse/key events and does not pause Game.
            -- Give the wait a visible explanation without adding an action callback.
            local frame=self.sdlext.buildTextDialog('存档 / 读档','正在处理，请稍候。')
            frame:pospx((pane.w-frame.w)/2,(pane.h-frame.h)/2):addTo(pane)
        end)
    elseif not busy and self.busy_owned and self.busy_quit and self:busy_is_top() then
        self.busy_quit()
    end
end
function UI:detach()
    self:sync_busy(false)
    self.root=nil
end
function UI:attach(root)
    if not root then return false,'no_root' end
    self.root=root
    return true
end
function UI:allowed(action,model)
    if not self.actions[action] then return false end
    model=model or self.controller:status()
    if model.action_allowed then return model.action_allowed[action]==true end
    if model.state~='idle' then return false end
    if action=='save_manual' then return true end
    return UI.can_read(model.slots[action=='load_auto' and 'auto' or 'manual'])==true
end
function UI:can_open()
    if self.busy_owned or self.sdlext.dialogVisible() then return false,'dialog_open' end
    local status=self.controller:status()
    return not self.controller.active and status.can_open~=false
end
function UI:open()
    local ok,reason=self:can_open()
    if not ok then return false,reason end
    local model,why=self.controller:open_panel()
    if not model then return false,why end
    local choices={{'保存到临时手动槽','save_manual'},{'读取临时手动槽','load_manual'},
        {'读取战前档','load_auto'},{'关闭',nil}}
    self.sdlext.showDialog(function(pane,quit)
        pane.dismissible=false
        pane.onDialogExit=function()
            local choice=choices[pane.response or 0]
            if choice and choice[2] and self:allowed(choice[2]) then
                self.actions[choice[2]]()
            end
        end
        local frame=self.sdlext.buildButtonDialog('存档 / 读档',function(scroll)
            local font=deco.uifont.tooltipTextLarge
            local text=UiWrappedText(UI.panel_text(model),font.font,font.set):widthpx(scroll.w):addTo(scroll)
            text.pixelWrap=true;text:rebuild()
        end,function(layout)
            for index,item in ipairs(choices) do
                local action=item[2]
                local enabled=not action or self:allowed(action,model)
                local button=self.sdlext.buildButton(item[1],'',function()
                    -- disabled is decorative in this loader; always consume and recheck.
                    if action and not self:allowed(action) then return true end
                    pane.response=index
                    quit()
                    return true
                end)
                button.disabled=not enabled
                button:addTo(layout)
            end
        end,{maxW=0.7*ScreenSizeX(),maxH=0.8*ScreenSizeY()})
        frame:pospx((pane.w-frame.w)/2,(pane.h-frame.h)/2):addTo(pane)
    end)
    return true
end
function UI:confirm(title,text,callback)
    self.sdlext.showConfirmDialog(title,text,function(response)
        callback(UI.confirm_accepted(response) and 1 or 2)
    end)
end
return UI
