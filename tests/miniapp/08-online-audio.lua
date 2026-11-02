-- API 5: single click pauses/resumes, double click changes stations.
assert(app.api_version >= 5 and audio, "Audio: API 5 required")
local stations = {
  {"北京新闻 MP3", "https://lhttp.qtfm.cn/live/339/64k.mp3"},
  {"中国之声 HLS", "https://ngcdn001.cnr.cn/live/zgzs/index.m3u8"}
}
local station, elapsed, click_at = 1, 0, nil
local state = "idle"
local function draw()
  screen.begin(0x101923)
  screen.text("在线电台", 12, 12, 0xffffff)
  screen.text(stations[station][1], 12, 48, 0x71e4ad)
  screen.text(state, 12, 80, 0xffffff)
  screen.text("单击暂停或恢复", 12, 128, 0xa5b5c5)
  screen.text("双击切台，长按退出", 12, 160, 0xa5b5c5)
  screen.text("唤醒交互后自动恢复", 12, 192, 0xa5b5c5)
  screen.present()
end
function on_start()
  assert(audio.play(stations[station][2]))
  draw()
end
function on_tick(dt)
  elapsed = elapsed + dt
  if click_at and elapsed - click_at > 300 then
    click_at = nil
    if state == "paused" then audio.resume()
    elseif state == "idle" or state == "error" then assert(audio.play(stations[station][2]))
    else audio.pause() end
  end
  local next_state = audio.get_state()
  if next_state ~= state then state = next_state; draw() end
end
function on_button_click(id)
  if id ~= "function" then return end
  if click_at and elapsed - click_at <= 300 then
    click_at = nil
    station = station % #stations + 1
    assert(audio.play(stations[station][2]))
    draw()
  else click_at = elapsed end
end
