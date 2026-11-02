-- Extended graphics on a 240x240 MINI screen.
-- No network, wall-clock, storage or audio dependencies.
assert(app.api_version >= 4 and screen and screen.circle and screen.line and screen.arc,
       "GRAPHICS: API 4 with extended drawing support required")
assert(app.width == 240 and app.height == 240, "GRAPHICS: 240x240 screen required")
local page, elapsed, redraw_ms = 1, 0, 0
local BG, PANEL, WHITE, GREEN = 0x101725, 0x202D40, 0xFFFFFF, 0x58DEB1
local function text(value, y, color)
    screen.text(value, 12, y, color or WHITE, {width=216, align="center"})
end
local function footer()
    text("单击换页，长按退出", 214, 0x92ADBF)
end
local function draw()
    -- Page 3 deliberately uses legacy order: text is always above shapes.
    screen.begin(BG, {ordered=page ~= 3})
    if page == 1 then
        screen.rect(12, 12, 216, 216, PANEL, {radius=24})
        screen.arc(120, 111, 69, 0, 360, 0x34465D, {width=10})
        screen.arc(120, 111, 69, 270, math.floor(elapsed / 35) % 360,
                   GREEN, {width=10, rounded=true})
        text("1/7 圆弧", 58, 0x92ADBF)
        text("平滑边缘", 100)
        screen.circle(120, 145, 7, GREEN)
        screen.line(40, 190, 200, 190, 0x92ADBF, {width=2, rounded=true})
        text("不应闪烁或留下残影", 192, 0x92ADBF)
        footer()
    elseif page == 2 then
        text("2/7 圆、线与透明度", 12)
        screen.rect(20, 46, 200, 72, PANEL, {radius=18, border_width=2, border_color=GREEN})
        screen.circle(75, 82, 26, GREEN)
        screen.circle(113, 82, 26, 0xFFAA55, {opacity=140})
        screen.circle(174, 82, 24, 0x88BBFF, {fill=false, border_width=5})
        screen.line(30, 143, 210, 163, GREEN, {width=8, rounded=true})
        screen.circle(30 + math.floor(elapsed / 30) % 180, 177, 10, 0xFFAA55, {opacity=150})
        text("半透明圆移动应无残影", 192, 0x92ADBF)
        footer()
    elseif page == 3 or page == 4 then
        text(page == 3 and "3/7 旧版叠放顺序" or "4/7 按调用顺序叠放", 12)
        -- Text first, rectangle second. Only legacy mode keeps the text visible.
        screen.text("这行先画的文字", 36, 88, WHITE)
        screen.rect(24, 70, 192, 60, 0x38654F, {radius=12})
        text(page == 3 and "绿色框内应看见文字" or "绿色框内不应有文字", 150, GREEN)
        text("后画的这行始终可见", 178)
        footer()
    elseif page == 5 then
        text("5/7 文字布局", 12)
        screen.rect(20, 46, 200, 126, PANEL, {radius=12})
        screen.text("左对齐", 28, 54, WHITE, {width=184, align="left"})
        screen.text("居中", 28, 78, GREEN, {width=184, align="center"})
        screen.text("右对齐", 28, 102, 0xFFAA55, {width=184, align="right"})
        screen.text("WRAP WORDS WRAP WORDS", 28, 126, WHITE, {width=130})
        -- No width: preserve legacy clipping rather than automatic wrapping.
        screen.text("CLIP:012345678901234567890123456789", 12, 180, 0x92ADBF)
        footer()
    else
        -- Exactly 256 shapes and 16 text runs. Page 6 repeatedly submits the
        -- same scene; page 7 changes every tile to expose the rendering cost.
        local shift = page == 7 and math.floor(elapsed / 40) * 97 or 0
        for i=0,255 do
            screen.rect(i % 16 * 15, math.floor(i / 16) * 15, 12, 12,
                        (i * 60251 + shift) % 0xFFFFFF, {radius=3})
        end
        screen.text(page == 6 and "6/7 静态满预算" or "7/7 动态满预算", 8, 8, WHITE)
        screen.text("256 图形 + 16 段文字", 8, 32, WHITE)
        for i=1,12 do
            screen.text(tostring(i), (i - 1) % 4 * 56 + 10,
                        math.floor((i - 1) / 4) * 40 + 70, WHITE)
        end
        screen.text(page == 6 and "相同画面不应重复刷屏" or "压力测试，不保证帧率", 8, 188, WHITE)
        screen.text("单击换页，长按退出", 8, 212, WHITE)
    end
    screen.present()
end
function on_start() draw() end
function on_tick(dt)
    assert(type(dt) == "number" and dt >= 0, "GRAPHICS: invalid tick")
    elapsed, redraw_ms = elapsed + dt, redraw_ms + dt
    if redraw_ms >= 40 and (page == 1 or page == 2 or page >= 6) then
        redraw_ms = 0
        draw()
    end
end
function on_button_click(button_id)
    if button_id ~= "function" then return end
    page, redraw_ms = page % 7 + 1, 0
    draw()
end
