-- API 5: four-voice music loop with manual and voice pause/resume.
-- Beethoven: Ode to Joy theme, based on Mutopia Project #528 (Public Domain).
-- Source: https://www.mutopiaproject.org/cgibin/piece-info.cgi?id=528
-- New bass/drum arrangement; 200 notes, 120 BPM, 16 seconds per loop.
assert(app.api_version >= 5 and synth and synth.play and synth.pause and synth.resume,
       "SYNTH: API 5 with synthesis support required")

local score = {
  {at=0, voice=1, hz=494, ms=475, wave="triangle", volume=100},
  {at=0, voice=2, hz=98, ms=330, wave="triangle", volume=100},
  {at=0, voice=3, hz=180, ms=20, wave="triangle", volume=85},
  {at=0, voice=4, hz=5000, ms=30, wave="noise", volume=18},
  {at=20, voice=3, hz=115, ms=40, wave="triangle", volume=76},
  {at=60, voice=3, hz=80, ms=85, wave="triangle", volume=68},
  {at=250, voice=4, hz=5000, ms=30, wave="noise", volume=12},
  {at=500, voice=1, hz=494, ms=475, wave="triangle", volume=100},
  {at=500, voice=4, hz=2300, ms=85, wave="noise", volume=45},
  {at=750, voice=2, hz=196, ms=180, wave="triangle", volume=100},
  {at=750, voice=3, hz=180, ms=20, wave="triangle", volume=85},
  {at=750, voice=4, hz=5000, ms=30, wave="noise", volume=12},
  {at=770, voice=3, hz=115, ms=40, wave="triangle", volume=76},
  {at=810, voice=3, hz=80, ms=85, wave="triangle", volume=68},
  {at=1000, voice=1, hz=523, ms=475, wave="triangle", volume=100},
  {at=1000, voice=2, hz=98, ms=330, wave="triangle", volume=100},
  {at=1000, voice=3, hz=180, ms=20, wave="triangle", volume=85},
  {at=1000, voice=4, hz=5000, ms=30, wave="noise", volume=18},
  {at=1020, voice=3, hz=115, ms=40, wave="triangle", volume=76},
  {at=1060, voice=3, hz=80, ms=85, wave="triangle", volume=68},
  {at=1250, voice=4, hz=5000, ms=30, wave="noise", volume=12},
  {at=1500, voice=1, hz=587, ms=475, wave="triangle", volume=100},
  {at=1500, voice=4, hz=2300, ms=85, wave="noise", volume=45},
  {at=1750, voice=2, hz=196, ms=250, wave="triangle", volume=100},
  {at=1750, voice=4, hz=5000, ms=30, wave="noise", volume=12},
  {at=2000, voice=1, hz=587, ms=475, wave="triangle", volume=100},
  {at=2000, voice=2, hz=147, ms=330, wave="triangle", volume=100},
  {at=2000, voice=3, hz=180, ms=20, wave="triangle", volume=85},
  {at=2000, voice=4, hz=5000, ms=30, wave="noise", volume=18},
  {at=2020, voice=3, hz=115, ms=40, wave="triangle", volume=76},
  {at=2060, voice=3, hz=80, ms=85, wave="triangle", volume=68},
  {at=2250, voice=4, hz=5000, ms=30, wave="noise", volume=12},
  {at=2500, voice=1, hz=523, ms=475, wave="triangle", volume=100},
  {at=2500, voice=4, hz=2300, ms=85, wave="noise", volume=45},
  {at=2750, voice=2, hz=294, ms=180, wave="triangle", volume=100},
  {at=2750, voice=4, hz=5000, ms=30, wave="noise", volume=12},
  {at=3000, voice=1, hz=494, ms=475, wave="triangle", volume=100},
  {at=3000, voice=2, hz=147, ms=330, wave="triangle", volume=100},
  {at=3000, voice=3, hz=180, ms=20, wave="triangle", volume=85},
  {at=3000, voice=4, hz=5000, ms=30, wave="noise", volume=18},
  {at=3020, voice=3, hz=115, ms=40, wave="triangle", volume=76},
  {at=3060, voice=3, hz=80, ms=85, wave="triangle", volume=68},
  {at=3250, voice=3, hz=180, ms=20, wave="triangle", volume=85},
  {at=3250, voice=4, hz=5000, ms=30, wave="noise", volume=12},
  {at=3270, voice=3, hz=115, ms=40, wave="triangle", volume=76},
  {at=3310, voice=3, hz=80, ms=85, wave="triangle", volume=68},
  {at=3500, voice=1, hz=440, ms=475, wave="triangle", volume=100},
  {at=3500, voice=4, hz=2300, ms=85, wave="noise", volume=45},
  {at=3750, voice=2, hz=294, ms=250, wave="triangle", volume=100},
  {at=3750, voice=4, hz=5000, ms=90, wave="noise", volume=12},
  {at=4000, voice=1, hz=392, ms=475, wave="triangle", volume=100},
  {at=4000, voice=2, hz=98, ms=330, wave="triangle", volume=100},
  {at=4000, voice=3, hz=180, ms=20, wave="triangle", volume=85},
  {at=4000, voice=4, hz=5000, ms=30, wave="noise", volume=18},
  {at=4020, voice=3, hz=115, ms=40, wave="triangle", volume=76},
  {at=4060, voice=3, hz=80, ms=85, wave="triangle", volume=68},
  {at=4250, voice=4, hz=5000, ms=30, wave="noise", volume=12},
  {at=4500, voice=1, hz=392, ms=475, wave="triangle", volume=100},
  {at=4500, voice=4, hz=2300, ms=85, wave="noise", volume=45},
  {at=4750, voice=2, hz=196, ms=180, wave="triangle", volume=100},
  {at=4750, voice=3, hz=180, ms=20, wave="triangle", volume=85},
  {at=4750, voice=4, hz=5000, ms=30, wave="noise", volume=12},
  {at=4770, voice=3, hz=115, ms=40, wave="triangle", volume=76},
  {at=4810, voice=3, hz=80, ms=85, wave="triangle", volume=68},
  {at=5000, voice=1, hz=440, ms=475, wave="triangle", volume=100},
  {at=5000, voice=2, hz=98, ms=330, wave="triangle", volume=100},
  {at=5000, voice=3, hz=180, ms=20, wave="triangle", volume=85},
  {at=5000, voice=4, hz=5000, ms=30, wave="noise", volume=18},
  {at=5020, voice=3, hz=115, ms=40, wave="triangle", volume=76},
  {at=5060, voice=3, hz=80, ms=85, wave="triangle", volume=68},
  {at=5250, voice=4, hz=5000, ms=30, wave="noise", volume=12},
  {at=5500, voice=1, hz=494, ms=475, wave="triangle", volume=100},
  {at=5500, voice=4, hz=2300, ms=85, wave="noise", volume=45},
  {at=5750, voice=2, hz=196, ms=250, wave="triangle", volume=100},
  {at=5750, voice=4, hz=5000, ms=30, wave="noise", volume=12},
  {at=6000, voice=1, hz=494, ms=725, wave="triangle", volume=100},
  {at=6000, voice=2, hz=147, ms=330, wave="triangle", volume=100},
  {at=6000, voice=3, hz=180, ms=20, wave="triangle", volume=85},
  {at=6000, voice=4, hz=5000, ms=30, wave="noise", volume=18},
  {at=6020, voice=3, hz=115, ms=40, wave="triangle", volume=76},
  {at=6060, voice=3, hz=80, ms=85, wave="triangle", volume=68},
  {at=6250, voice=4, hz=5000, ms=30, wave="noise", volume=12},
  {at=6500, voice=4, hz=2300, ms=85, wave="noise", volume=45},
  {at=6750, voice=1, hz=440, ms=225, wave="triangle", volume=100},
  {at=6750, voice=2, hz=294, ms=180, wave="triangle", volume=100},
  {at=6750, voice=4, hz=5000, ms=30, wave="noise", volume=12},
  {at=7000, voice=1, hz=440, ms=975, wave="triangle", volume=100},
  {at=7000, voice=2, hz=147, ms=330, wave="triangle", volume=100},
  {at=7000, voice=3, hz=180, ms=20, wave="triangle", volume=85},
  {at=7000, voice=4, hz=5000, ms=30, wave="noise", volume=18},
  {at=7020, voice=3, hz=115, ms=40, wave="triangle", volume=76},
  {at=7060, voice=3, hz=80, ms=85, wave="triangle", volume=68},
  {at=7250, voice=3, hz=180, ms=20, wave="triangle", volume=85},
  {at=7250, voice=4, hz=5000, ms=30, wave="noise", volume=12},
  {at=7270, voice=3, hz=115, ms=40, wave="triangle", volume=76},
  {at=7310, voice=3, hz=80, ms=85, wave="triangle", volume=68},
  {at=7500, voice=4, hz=2300, ms=85, wave="noise", volume=45},
  {at=7625, voice=4, hz=2300, ms=40, wave="noise", volume=22},
  {at=7750, voice=2, hz=294, ms=250, wave="triangle", volume=100},
  {at=7750, voice=4, hz=5000, ms=90, wave="noise", volume=12},
  {at=7875, voice=4, hz=2300, ms=40, wave="noise", volume=22},
  {at=8000, voice=1, hz=494, ms=475, wave="triangle", volume=100},
  {at=8000, voice=2, hz=98, ms=330, wave="triangle", volume=100},
  {at=8000, voice=3, hz=180, ms=20, wave="triangle", volume=85},
  {at=8000, voice=4, hz=5000, ms=30, wave="noise", volume=18},
  {at=8020, voice=3, hz=115, ms=40, wave="triangle", volume=76},
  {at=8060, voice=3, hz=80, ms=85, wave="triangle", volume=68},
  {at=8250, voice=4, hz=5000, ms=30, wave="noise", volume=12},
  {at=8500, voice=1, hz=494, ms=475, wave="triangle", volume=100},
  {at=8500, voice=4, hz=2300, ms=85, wave="noise", volume=45},
  {at=8750, voice=2, hz=196, ms=180, wave="triangle", volume=100},
  {at=8750, voice=3, hz=180, ms=20, wave="triangle", volume=85},
  {at=8750, voice=4, hz=5000, ms=30, wave="noise", volume=12},
  {at=8770, voice=3, hz=115, ms=40, wave="triangle", volume=76},
  {at=8810, voice=3, hz=80, ms=85, wave="triangle", volume=68},
  {at=9000, voice=1, hz=523, ms=475, wave="triangle", volume=100},
  {at=9000, voice=2, hz=98, ms=330, wave="triangle", volume=100},
  {at=9000, voice=3, hz=180, ms=20, wave="triangle", volume=85},
  {at=9000, voice=4, hz=5000, ms=30, wave="noise", volume=18},
  {at=9020, voice=3, hz=115, ms=40, wave="triangle", volume=76},
  {at=9060, voice=3, hz=80, ms=85, wave="triangle", volume=68},
  {at=9250, voice=4, hz=5000, ms=30, wave="noise", volume=12},
  {at=9500, voice=1, hz=587, ms=475, wave="triangle", volume=100},
  {at=9500, voice=4, hz=2300, ms=85, wave="noise", volume=45},
  {at=9750, voice=2, hz=196, ms=250, wave="triangle", volume=100},
  {at=9750, voice=4, hz=5000, ms=30, wave="noise", volume=12},
  {at=10000, voice=1, hz=587, ms=475, wave="triangle", volume=100},
  {at=10000, voice=2, hz=147, ms=330, wave="triangle", volume=100},
  {at=10000, voice=3, hz=180, ms=20, wave="triangle", volume=85},
  {at=10000, voice=4, hz=5000, ms=30, wave="noise", volume=18},
  {at=10020, voice=3, hz=115, ms=40, wave="triangle", volume=76},
  {at=10060, voice=3, hz=80, ms=85, wave="triangle", volume=68},
  {at=10250, voice=4, hz=5000, ms=30, wave="noise", volume=12},
  {at=10500, voice=1, hz=523, ms=475, wave="triangle", volume=100},
  {at=10500, voice=4, hz=2300, ms=85, wave="noise", volume=45},
  {at=10750, voice=2, hz=294, ms=180, wave="triangle", volume=100},
  {at=10750, voice=4, hz=5000, ms=30, wave="noise", volume=12},
  {at=11000, voice=1, hz=494, ms=475, wave="triangle", volume=100},
  {at=11000, voice=2, hz=147, ms=330, wave="triangle", volume=100},
  {at=11000, voice=3, hz=180, ms=20, wave="triangle", volume=85},
  {at=11000, voice=4, hz=5000, ms=30, wave="noise", volume=18},
  {at=11020, voice=3, hz=115, ms=40, wave="triangle", volume=76},
  {at=11060, voice=3, hz=80, ms=85, wave="triangle", volume=68},
  {at=11250, voice=3, hz=180, ms=20, wave="triangle", volume=85},
  {at=11250, voice=4, hz=5000, ms=30, wave="noise", volume=12},
  {at=11270, voice=3, hz=115, ms=40, wave="triangle", volume=76},
  {at=11310, voice=3, hz=80, ms=85, wave="triangle", volume=68},
  {at=11500, voice=1, hz=440, ms=475, wave="triangle", volume=100},
  {at=11500, voice=4, hz=2300, ms=85, wave="noise", volume=45},
  {at=11750, voice=2, hz=294, ms=250, wave="triangle", volume=100},
  {at=11750, voice=4, hz=5000, ms=90, wave="noise", volume=12},
  {at=12000, voice=1, hz=392, ms=475, wave="triangle", volume=100},
  {at=12000, voice=2, hz=131, ms=330, wave="triangle", volume=100},
  {at=12000, voice=3, hz=180, ms=20, wave="triangle", volume=85},
  {at=12000, voice=4, hz=5000, ms=30, wave="noise", volume=18},
  {at=12020, voice=3, hz=115, ms=40, wave="triangle", volume=76},
  {at=12060, voice=3, hz=80, ms=85, wave="triangle", volume=68},
  {at=12250, voice=4, hz=5000, ms=30, wave="noise", volume=12},
  {at=12500, voice=1, hz=392, ms=475, wave="triangle", volume=100},
  {at=12500, voice=4, hz=2300, ms=85, wave="noise", volume=45},
  {at=12750, voice=2, hz=262, ms=180, wave="triangle", volume=100},
  {at=12750, voice=3, hz=180, ms=20, wave="triangle", volume=85},
  {at=12750, voice=4, hz=5000, ms=30, wave="noise", volume=12},
  {at=12770, voice=3, hz=115, ms=40, wave="triangle", volume=76},
  {at=12810, voice=3, hz=80, ms=85, wave="triangle", volume=68},
  {at=13000, voice=1, hz=440, ms=475, wave="triangle", volume=100},
  {at=13000, voice=2, hz=131, ms=330, wave="triangle", volume=100},
  {at=13000, voice=3, hz=180, ms=20, wave="triangle", volume=85},
  {at=13000, voice=4, hz=5000, ms=30, wave="noise", volume=18},
  {at=13020, voice=3, hz=115, ms=40, wave="triangle", volume=76},
  {at=13060, voice=3, hz=80, ms=85, wave="triangle", volume=68},
  {at=13250, voice=4, hz=5000, ms=30, wave="noise", volume=12},
  {at=13500, voice=1, hz=494, ms=475, wave="triangle", volume=100},
  {at=13500, voice=4, hz=2300, ms=85, wave="noise", volume=45},
  {at=13750, voice=2, hz=262, ms=250, wave="triangle", volume=100},
  {at=13750, voice=4, hz=5000, ms=30, wave="noise", volume=12},
  {at=14000, voice=1, hz=440, ms=725, wave="triangle", volume=100},
  {at=14000, voice=2, hz=98, ms=330, wave="triangle", volume=100},
  {at=14000, voice=3, hz=180, ms=20, wave="triangle", volume=85},
  {at=14000, voice=4, hz=5000, ms=30, wave="noise", volume=18},
  {at=14020, voice=3, hz=115, ms=40, wave="triangle", volume=76},
  {at=14060, voice=3, hz=80, ms=85, wave="triangle", volume=68},
  {at=14250, voice=4, hz=5000, ms=30, wave="noise", volume=12},
  {at=14500, voice=4, hz=2300, ms=85, wave="noise", volume=45},
  {at=14750, voice=1, hz=392, ms=225, wave="triangle", volume=100},
  {at=14750, voice=2, hz=196, ms=180, wave="triangle", volume=100},
  {at=14750, voice=4, hz=5000, ms=30, wave="noise", volume=12},
  {at=15000, voice=1, hz=392, ms=975, wave="triangle", volume=100},
  {at=15000, voice=2, hz=98, ms=330, wave="triangle", volume=100},
  {at=15000, voice=3, hz=180, ms=20, wave="triangle", volume=85},
  {at=15000, voice=4, hz=5000, ms=30, wave="noise", volume=18},
  {at=15020, voice=3, hz=115, ms=40, wave="triangle", volume=76},
  {at=15060, voice=3, hz=80, ms=85, wave="triangle", volume=68},
  {at=15250, voice=3, hz=180, ms=20, wave="triangle", volume=85},
  {at=15250, voice=4, hz=5000, ms=30, wave="noise", volume=12},
  {at=15270, voice=3, hz=115, ms=40, wave="triangle", volume=76},
  {at=15310, voice=3, hz=80, ms=85, wave="triangle", volume=68},
  {at=15500, voice=4, hz=2300, ms=85, wave="noise", volume=45},
  {at=15750, voice=2, hz=196, ms=250, wave="triangle", volume=100},
  {at=15750, voice=4, hz=5000, ms=90, wave="noise", volume=12},
}
local paused = false

local function draw()
    if not screen then
        return
    end
    screen.begin(0x101820)
    screen.text("欢乐颂 · 多声部", 16, 24, 0xffffff)
    screen.text(paused and "已暂停" or "播放中", 16, 64, 0x66ccff)
    screen.text("16 秒循环 / 旋律、低音、鼓组", 16, 88, 0xa5b5c5)
    screen.text("单击暂停/恢复", 16, 112, 0xa5b5c5)
    screen.text("长按退出", 16, 144, 0xa5b5c5)
    screen.present()
end

local function set_paused(value)
    paused = value
    if paused then
        synth.pause()
    else
        synth.resume()
    end
    draw()
end

function on_start()
    assert(synth.play(score, {loop=true}), "synth playback unavailable")
    draw()
end

function on_button_click(id)
    if id == "function" then
        set_paused(not paused)
    end
end

function on_tick(dt_ms)
end

function get_state()
    return paused and "paused" or "playing"
end

function get_actions()
    return {
        pause = {description="暂停音效", parameters={}},
        resume = {description="恢复音效", parameters={}},
    }
end

function on_action(name, args)
    if name == "pause" then
        set_paused(true)
    elseif name == "resume" then
        set_paused(false)
    else
        return {ok=false, message="不支持的操作"}
    end
    return {ok=true, message=get_state()}
end
