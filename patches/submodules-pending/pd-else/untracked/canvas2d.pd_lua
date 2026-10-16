-- canvas2d.pd_lua - Universal 2D GPU Drawing Canvas for PlugData
-- Driven entirely by Pure Data patch messages via NanoVG
-- Author: Antigravity & Jupiter Moll

local c = pd.Class:new():register("canvas2d")

function c:initialize(sel, atoms)
    self.inlets = 1
    self.outlets = 2 -- Outlet 1: list (mouse x y), Outlet 2: bang (click)

    self.width = tonumber(atoms[1]) or 240
    self.height = tonumber(atoms[2]) or 240
    self:set_size(self.width, self.height)

    self.commands = {}
    self.current_color = {220, 220, 230, 1.0}
    self.bg_color = {22, 24, 30, 1.0}

    return true
end

-- ================= Pure Data Inlet Messages =================

-- [clear(
function c:in_1_clear()
    self.commands = {}
    self:repaint()
end

-- [color r g b [a]( (0-255 or 0.0-1.0)
function c:in_1_color(atoms)
    local r = tonumber(atoms[1]) or 255
    local g = tonumber(atoms[2]) or 255
    local b = tonumber(atoms[3]) or 255
    local a = tonumber(atoms[4]) or 1.0
    if r <= 1.0 and g <= 1.0 and b <= 1.0 and (atoms[1] or 0) <= 1.0 and (atoms[2] or 0) <= 1.0 then
        r, g, b = r * 255, g * 255, b * 255
    end
    self.current_color = {r, g, b, a}
end

-- [bgcolor r g b [a](
function c:in_1_bgcolor(atoms)
    local r = tonumber(atoms[1]) or 22
    local g = tonumber(atoms[2]) or 24
    local b = tonumber(atoms[3]) or 30
    local a = tonumber(atoms[4]) or 1.0
    self.bg_color = {r, g, b, a}
    self:repaint()
end

-- [circle x y r(
function c:in_1_circle(atoms)
    table.insert(self.commands, {
        type = "fill_circle",
        x = tonumber(atoms[1]) or 0,
        y = tonumber(atoms[2]) or 0,
        r = tonumber(atoms[3]) or 10,
        color = {self.current_color[1], self.current_color[2], self.current_color[3], self.current_color[4]}
    })
    self:repaint()
end

-- [stroke_circle x y r [width](
function c:in_1_stroke_circle(atoms)
    table.insert(self.commands, {
        type = "stroke_circle",
        x = tonumber(atoms[1]) or 0,
        y = tonumber(atoms[2]) or 0,
        r = tonumber(atoms[3]) or 10,
        width = tonumber(atoms[4]) or 1.5,
        color = {self.current_color[1], self.current_color[2], self.current_color[3], self.current_color[4]}
    })
    self:repaint()
end

-- [rect x y w h(
function c:in_1_rect(atoms)
    table.insert(self.commands, {
        type = "fill_rect",
        x = tonumber(atoms[1]) or 0,
        y = tonumber(atoms[2]) or 0,
        w = tonumber(atoms[3]) or 20,
        h = tonumber(atoms[4]) or 20,
        color = {self.current_color[1], self.current_color[2], self.current_color[3], self.current_color[4]}
    })
    self:repaint()
end

-- [stroke_rect x y w h [width](
function c:in_1_stroke_rect(atoms)
    table.insert(self.commands, {
        type = "stroke_rect",
        x = tonumber(atoms[1]) or 0,
        y = tonumber(atoms[2]) or 0,
        w = tonumber(atoms[3]) or 20,
        h = tonumber(atoms[4]) or 20,
        width = tonumber(atoms[5]) or 1.5,
        color = {self.current_color[1], self.current_color[2], self.current_color[3], self.current_color[4]}
    })
    self:repaint()
end

-- [line x1 y1 x2 y2 [width](
function c:in_1_line(atoms)
    table.insert(self.commands, {
        type = "line",
        x1 = tonumber(atoms[1]) or 0,
        y1 = tonumber(atoms[2]) or 0,
        x2 = tonumber(atoms[3]) or 10,
        y2 = tonumber(atoms[4]) or 10,
        width = tonumber(atoms[5]) or 1.5,
        color = {self.current_color[1], self.current_color[2], self.current_color[3], self.current_color[4]}
    })
    self:repaint()
end

-- [text string x y [w] [size](
function c:in_1_text(atoms)
    local txt = tostring(atoms[1] or "")
    local x = tonumber(atoms[2]) or 10
    local y = tonumber(atoms[3]) or 10
    local w = tonumber(atoms[4]) or (self.width - 20)
    local sz = tonumber(atoms[5]) or 13
    table.insert(self.commands, {
        type = "text",
        txt = txt,
        x = x,
        y = y,
        w = w,
        size = sz,
        color = {self.current_color[1], self.current_color[2], self.current_color[3], self.current_color[4]}
    })
    self:repaint()
end

-- [redraw( or [bang(
function c:in_1_redraw()
    self:repaint()
end
function c:in_1_bang()
    self:repaint()
end

-- ================= Mouse Interaction =================

function c:mouse_down(x, y)
    self:outlet(1, "list", { x / self.width, 1.0 - (y / self.height) })
    self:outlet(2, "bang", {})
end

function c:mouse_drag(x, y)
    local norm_x = math.max(0.0, math.min(1.0, x / self.width))
    local norm_y = math.max(0.0, math.min(1.0, 1.0 - (y / self.height)))
    self:outlet(1, "list", { norm_x, norm_y })
end

-- ================= NanoVG GPU Rendering =================

function c:paint(g)
    -- Dark background container
    g:set_color(self.bg_color[1], self.bg_color[2], self.bg_color[3], self.bg_color[4])
    g:fill_rounded_rect(0, 0, self.width, self.height, 6)

    -- Subtle border
    g:set_color(55, 60, 75, 1.0)
    g:stroke_rounded_rect(0, 0, self.width, self.height, 6, 1)

    -- Draw user shapes
    for _, cmd in ipairs(self.commands) do
        g:set_color(cmd.color[1], cmd.color[2], cmd.color[3], cmd.color[4])

        if cmd.type == "fill_circle" then
            g:fill_ellipse(cmd.x - cmd.r, cmd.y - cmd.r, cmd.r * 2, cmd.r * 2)
        elseif cmd.type == "stroke_circle" then
            g:stroke_ellipse(cmd.x - cmd.r, cmd.y - cmd.r, cmd.r * 2, cmd.r * 2, cmd.width)
        elseif cmd.type == "fill_rect" then
            g:fill_rect(cmd.x, cmd.y, cmd.w, cmd.h)
        elseif cmd.type == "stroke_rect" then
            g:stroke_rect(cmd.x, cmd.y, cmd.w, cmd.h, cmd.width)
        elseif cmd.type == "line" then
            g:draw_line(cmd.x1, cmd.y1, cmd.x2, cmd.y2, cmd.width)
        elseif cmd.type == "text" then
            g:draw_text(cmd.txt, cmd.x, cmd.y, cmd.w, cmd.size)
        end
    end
end
