-- API 5: install this app, then call ls.built_in.miniapp_describe / ls.built_in.miniapp_action via MCP.
local remaining_ms = 25 * 60 * 1000
local paused = true
local last_second = -1

local function draw()
    if not screen then return end
    local seconds = math.ceil(remaining_ms / 1000)
    screen.begin(0x101820)
    screen.text("Voice timer", 16, 24, 0xffffff)
    screen.text(string.format("%02d:%02d", seconds // 60, seconds % 60), 16, 64, 0x66ccff)
    screen.text(paused and "Paused" or "Running", 16, 104, 0xffffff)
    screen.present()
    last_second = seconds
end

function on_start() draw() end
function on_tick(dt_ms)
    if not paused then remaining_ms = math.max(0, remaining_ms - dt_ms) end
    if remaining_ms == 0 then paused = true end
    if math.ceil(remaining_ms / 1000) ~= last_second then draw() end
end
function on_button_click(id)
    if id == "function" then paused = not paused; draw() end
end

function get_state()
    return string.format("倒计时%s，还剩 %d 秒", paused and "已暂停" or "进行中", math.ceil(remaining_ms / 1000))
end

function get_actions()
    return {
        pause = {description = "暂停倒计时", parameters = {}},
        resume = {description = "继续倒计时", parameters = {}},
        set_duration = {
            description = "设置专注分钟数，重置并暂停倒计时",
            parameters = {
                minutes = {type = "integer", description = "专注分钟数", required = true, minimum = 1, maximum = 180}
            }
        }
    }
end

function on_action(name, args)
    if name == "pause" then paused = true
    elseif name == "resume" then
        if remaining_ms == 0 then return {ok = false, message = "倒计时已结束，请先设置时长"} end
        paused = false
    elseif name == "set_duration" then remaining_ms = args.minutes * 60000; paused = true
    else return {ok = false, message = "不支持的操作"} end
    draw()
    return {ok = true, message = get_state()}
end
