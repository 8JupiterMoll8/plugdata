-- xypad.pd_lua: Sleek NanoVG Vector Touchpad for PlugData
-- Direct X/Y modulation controller with glowing crosshair & coordinates
local pad = pd.Class:new():register("xypad")

function pad:initialize(sel, atoms)
    self.inlets = 1
    self.outlets = 3 -- 1: list (norm_x norm_y), 2: float (x), 3: float (y)

    self.width = tonumber(atoms[1]) or 160
    self.height = tonumber(atoms[2]) or 160
    self:set_size(self.width, self.height)

    self.norm_x = 0.5
    self.norm_y = 0.5
    self.pos_x = self.width * 0.5
    self.pos_y = self.height * 0.5
    self.is_down = false

    return true
end

function pad:postinitialize()
    self:set_size(self.width, self.height)
end

function pad:mouse_down(x, y)
    self.is_down = true
    self:update_pos(x, y)
end

function pad:mouse_drag(x, y)
    if self.is_down then
        self:update_pos(x, y)
    end
end

function pad:mouse_up(x, y)
    self.is_down = false
    self:repaint()
end

function pad:update_pos(x, y)
    self.pos_x = math.max(0, math.min(self.width, x))
    self.pos_y = math.max(0, math.min(self.height, y))
    self.norm_x = self.pos_x / self.width
    self.norm_y = 1.0 - (self.pos_y / self.height)

    -- Dispatch outlets
    self:outlet(3, "float", { self.norm_y })
    self:outlet(2, "float", { self.norm_x })
    self:outlet(1, "list", { self.norm_x, self.norm_y })
    self:repaint()
end

function pad:paint(g)
    -- 1. Base dark container
    g:set_color(24, 28, 36, 1.0)
    g:fill_all()

    -- 2. Sleek rounded panel with cyber border
    g:set_color(18, 22, 28, 1.0)
    g:fill_rounded_rect(0, 0, self.width, self.height, 6)
    
    g:set_color(45, 52, 65, 1.0)
    g:stroke_rounded_rect(0, 0, self.width, self.height, 6, 1.0)

    -- 3. Subtle grid lines (4x4)
    g:set_color(35, 42, 54, 0.6)
    for i = 1, 3 do
        local gx = (self.width / 4) * i
        local gy = (self.height / 4) * i
        g:draw_line(gx, 4, gx, self.height - 4, 1.0)
        g:draw_line(4, gy, self.width - 4, gy, 1.0)
    end

    -- 4. Crosshairs to puck position
    g:set_color(0, 229, 255, 0.35)
    g:draw_line(self.pos_x, 2, self.pos_x, self.height - 2, 1.0)
    g:draw_line(2, self.pos_y, self.width - 2, self.pos_y, 1.0)

    -- 5. Glowing Puck Core
    if self.is_down then
        -- Active glow halo
        g:set_color(0, 229, 255, 0.25)
        g:fill_ellipse(self.pos_x - 14, self.pos_y - 14, 28, 28)
        g:set_color(255, 179, 0, 0.9)
    else
        g:set_color(0, 229, 255, 0.8)
    end

    -- Solid inner puck
    g:fill_ellipse(self.pos_x - 6, self.pos_y - 6, 12, 12)
    g:set_color(255, 255, 255, 1.0)
    g:stroke_ellipse(self.pos_x - 6, self.pos_y - 6, 12, 12, 1.5)

    -- 6. Coordinate readout in corner
    g:set_color(140, 160, 190, 0.8)
    local txt = string.format("X: %.2f  Y: %.2f", self.norm_x, self.norm_y)
    g:draw_text(txt, 10, self.height - 18, self.width - 20, 10)
end
