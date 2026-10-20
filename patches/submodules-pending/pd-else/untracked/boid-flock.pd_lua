-- boid-flock.pd_lua - Nature of Code Ch.6 flocking for PlugData pd-lua
-- Namespaced (no builtin collision), 100 FPS, cruise propulsion, 4-param inlets
-- Outlets: 1:list {norm_x norm_y dispersion chaos} 2:float avg_speed 3:bang click
-- Inlets: 1:speed/fps/reload/bang 2:spread/sep 3:gravity/coh 4:fps

local boid_flock = pd.Class:new():register("boid-flock")

function boid_flock:initialize(sel, atoms)
    self.inlets = 4
    self.outlets = 3

    self._name = "boid-flock"
    self._scriptname = "boid-flock.pd_lua"

    self.width = 300
    self.height = 300
    self:set_size(self.width, self.height)

    self.num_boids = 26
    self.boids = {}
    self.max_speed = 5.6
    self.max_force = 0.28
    self.sep_force = 3.4
    self.coh_force = 0.02
    self.frame_delay = 10 -- 100 FPS

    for i = 1, self.num_boids do
        local r_tier = math.random()
        local base_r = 2.8
        if r_tier < 0.15 then
            base_r = 6.5 + math.random() * 1.5
        elseif r_tier < 0.50 then
            base_r = 4.2 + math.random() * 1.2
        else
            base_r = 2.4 + math.random() * 0.8
        end
        table.insert(self.boids, {
            x = math.random(35, self.width - 35),
            y = math.random(35, self.height - 35),
            vx = (math.random() - 0.5) * 4.0,
            vy = (math.random() - 0.5) * 4.0,
            radius = base_r,
            wander = math.random() * 6.28
        })
    end

    self.mouse_x = self.width / 2
    self.mouse_y = self.height / 2
    self.is_mouse_down = false
    return true
end

function boid_flock:postinitialize()
    self:set_size(self.width, self.height)
    self.clock = pd.Clock:new():register(self, "tick")
    self.clock:delay(self.frame_delay or 10)
end

function boid_flock:finalize()
    if self.clock then self.clock:destruct() end
end

-- Hot-reload: [reload( -> live script swap, no restart
function boid_flock:in_1_reload()
    if self.dofilex and self._scriptname then
        self:dofilex(self._scriptname)
        pd.post("[" .. tostring(self._name) .. "] Live reloaded script: " .. tostring(self._scriptname))
    end
end

function boid_flock:set_speed(new_speed)
    local val = type(new_speed) == "table" and new_speed[1] or new_speed
    val = tonumber(val)
    if not val then return end
    local old = self.max_speed or 5.6
    self.max_speed = math.max(0.5, math.min(30.0, val))
    if old > 0 and self.boids and #self.boids > 0 then
        local ratio = self.max_speed / old
        for i = 1, #self.boids do
            self.boids[i].vx = self.boids[i].vx * ratio
            self.boids[i].vy = self.boids[i].vy * ratio
        end
    end
end

function boid_flock:set_sep(val)
    local v = type(val) == "table" and val[1] or val
    v = tonumber(v)
    if not v then return end
    if v > 25.0 then v = v * 0.2 end
    self.sep_force = math.max(0.2, math.min(25.0, v))
end

function boid_flock:set_coh(val)
    local v = type(val) == "table" and val[1] or val
    v = tonumber(v)
    if not v then return end
    if v > 1.0 then v = v * 0.001 end
    self.coh_force = math.max(0.001, math.min(0.25, v))
end

function boid_flock:set_fps(val)
    local v = type(val) == "table" and val[1] or val
    v = tonumber(v)
    if not v then return end
    local target_fps = math.max(20, math.min(144, v))
    self.frame_delay = math.max(6, math.floor(1000 / target_fps))
end

function boid_flock:in_1_float(f) self:set_speed(f) end
function boid_flock:in_1_speed(atoms) self:set_speed(atoms) end
function boid_flock:in_1_sep(atoms) self:set_sep(atoms) end
function boid_flock:in_1_coh(atoms) self:set_coh(atoms) end
function boid_flock:in_1_fps(atoms) self:set_fps(atoms) end
function boid_flock:in_1_bang()
    for i = 1, #self.boids do
        local angle = math.random() * 6.28
        local pwr = 3.0 + math.random() * self.max_speed
        self.boids[i].vx = math.cos(angle) * pwr
        self.boids[i].vy = math.sin(angle) * pwr
    end
end

function boid_flock:in_1(sel, atoms)
    if sel == "speed" then self:set_speed(atoms)
    elseif sel == "sep" then self:set_sep(atoms)
    elseif sel == "coh" then self:set_coh(atoms)
    elseif sel == "fps" then self:set_fps(atoms)
    elseif sel == "bang" then self:in_1_bang()
    end
end

function boid_flock:in_2_float(f) self:set_sep(f) end
function boid_flock:in_2_sep(atoms) self:set_sep(atoms) end
function boid_flock:in_2(sel, atoms)
    local v = sel == "sep" and atoms or sel
    self:set_sep(v)
end

function boid_flock:in_3_float(f) self:set_coh(f) end
function boid_flock:in_3_coh(atoms) self:set_coh(atoms) end
function boid_flock:in_3(sel, atoms)
    local v = sel == "coh" and atoms or sel
    self:set_coh(v)
end

function boid_flock:in_4_float(f) self:set_fps(f) end
function boid_flock:in_4_fps(atoms) self:set_fps(atoms) end
function boid_flock:in_4(sel, atoms)
    local v = sel == "fps" and atoms or sel
    self:set_fps(v)
end

function boid_flock:mouse_down(x, y)
    self.is_mouse_down = true
    self.mouse_x = x
    self.mouse_y = y
    self:outlet(3, "bang", {})
end

function boid_flock:mouse_drag(x, y)
    self.mouse_x = x
    self.mouse_y = y
end

function boid_flock:mouse_up(x, y)
    self.is_mouse_down = false
end

function boid_flock:tick()
    local sum_x, sum_y = 0, 0
    local sum_vx, sum_vy = 0, 0
    local total_speed = 0
    local n = #self.boids

    for i = 1, n do
        local b1 = self.boids[i]
        local sep_x, sep_y, sep_count = 0, 0, 0
        local ali_x, ali_y, ali_count = 0, 0, 0
        local coh_x, coh_y, coh_count = 0, 0, 0

        for j = 1, n do
            if i ~= j then
                local b2 = self.boids[j]
                local dx = b1.x - b2.x
                local dy = b1.y - b2.y
                local dist_sq = dx * dx + dy * dy
                if dist_sq > 0 and dist_sq < 1764 then
                    local dist = math.sqrt(dist_sq)
                    sep_x = sep_x + (dx / dist) / dist
                    sep_y = sep_y + (dy / dist) / dist
                    sep_count = sep_count + 1
                end
                if dist_sq > 0 and dist_sq < 4900 then
                    ali_x = ali_x + b2.vx
                    ali_y = ali_y + b2.vy
                    ali_count = ali_count + 1
                    coh_x = coh_x + b2.x
                    coh_y = coh_y + b2.y
                    coh_count = coh_count + 1
                end
            end
        end

        local ax, ay = 0, 0
        if sep_count > 0 then
            local sf = self.sep_force or 3.4
            ax = ax + sep_x * sf
            ay = ay + sep_y * sf
        end
        if ali_count > 0 then
            ax = ax + ((ali_x / ali_count) - b1.vx) * 0.08
            ay = ay + ((ali_y / ali_count) - b1.vy) * 0.08
        end
        if coh_count > 0 then
            local cf = self.coh_force or 0.02
            ax = ax + ((coh_x / coh_count) - b1.x) * cf
            ay = ay + ((coh_y / coh_count) - b1.y) * cf
        end

        b1.wander = b1.wander + 0.12
        ax = ax + math.cos(b1.wander) * 0.10
        ay = ay + math.sin(b1.wander) * 0.10

        if self.is_mouse_down then
            local mdx = self.mouse_x - b1.x
            local mdy = self.mouse_y - b1.y
            local mdist = math.sqrt(mdx * mdx + mdy * mdy) + 8
            local mforce = math.min(5.0, 120 / mdist)
            ax = ax + (mdx / mdist) * mforce - (mdy / mdist) * (mforce * 0.35)
            ay = ay + (mdy / mdist) * mforce + (mdx / mdist) * (mforce * 0.35)
        end

        b1.vx = b1.vx + ax
        b1.vy = b1.vy + ay

        local spd = math.sqrt(b1.vx * b1.vx + b1.vy * b1.vy)
        if spd > 0.001 then
            local new_spd = spd + (self.max_speed - spd) * 0.20
            b1.vx = (b1.vx / spd) * new_spd
            b1.vy = (b1.vy / spd) * new_spd
            spd = new_spd
        else
            local angle = math.random() * 6.28
            b1.vx = math.cos(angle) * self.max_speed
            b1.vy = math.sin(angle) * self.max_speed
            spd = self.max_speed
        end

        b1.x = b1.x + b1.vx
        b1.y = b1.y + b1.vy

        if b1.x < 22 then b1.vx = b1.vx + 0.4 end
        if b1.x > self.width - 22 then b1.vx = b1.vx - 0.4 end
        if b1.y < 22 then b1.vy = b1.vy + 0.4 end
        if b1.y > self.height - 22 then b1.vy = b1.vy - 0.4 end
        if b1.x < 8 then b1.x = 8; b1.vx = math.abs(b1.vx) * 0.8 end
        if b1.x > self.width - 8 then b1.x = self.width - 8; b1.vx = -math.abs(b1.vx) * 0.8 end
        if b1.y < 8 then b1.y = 8; b1.vy = math.abs(b1.vy) * 0.8 end
        if b1.y > self.height - 8 then b1.y = self.height - 8; b1.vy = -math.abs(b1.vy) * 0.8 end

        sum_x = sum_x + b1.x
        sum_y = sum_y + b1.y
        sum_vx = sum_vx + b1.vx
        sum_vy = sum_vy + b1.vy
        total_speed = total_speed + spd
    end

    local cx = sum_x / n
    local cy = sum_y / n
    local sum_dist = 0
    for i = 1, n do
        local b = self.boids[i]
        local dx = b.x - cx
        local dy = b.y - cy
        sum_dist = sum_dist + math.sqrt(dx * dx + dy * dy)
    end
    local dispersion = math.max(0.0, math.min(1.0, (sum_dist / n) / 100.0))
    local net_v = math.sqrt(sum_vx * sum_vx + sum_vy * sum_vy)
    local coherence = total_speed > 0.001 and (net_v / total_speed) or 1.0
    local chaos = math.max(0.0, math.min(1.0, 1.0 - coherence))

    local norm_x = math.max(0.0, math.min(1.0, cx / self.width))
    local norm_y = math.max(0.0, math.min(1.0, 1.0 - (cy / self.height)))
    local avg_speed = math.max(0.0, math.min(1.0, (total_speed / n) / self.max_speed))

    self:outlet(1, "list", { norm_x, norm_y, dispersion, chaos })
    self:outlet(2, "float", { avg_speed })

    self:repaint()
    self.clock:delay(self.frame_delay or 10)
end

function boid_flock:paint(g)
    -- Framebuffer anchor (mandatory for NanoVG stability)
    g:set_color(18, 20, 26, 1.0)
    g:fill_all()

    g:set_color(22, 24, 29, 1.0)
    g:fill_rounded_rect(0, 0, self.width, self.height, 8)
    g:set_color(45, 52, 68, 0.8)
    g:stroke_rounded_rect(0, 0, self.width, self.height, 8, 1.0)

    local n = #self.boids
    for i = 1, n do
        for j = i + 1, n do
            local dx = self.boids[j].x - self.boids[i].x
            local dy = self.boids[j].y - self.boids[i].y
            local dist_sq = dx * dx + dy * dy
            if dist_sq < 3025 then
                local d = math.sqrt(dist_sq)
                local alpha = (1.0 - (d / 55.0)) * 0.45
                g:set_color(50, 100, 150, alpha)
                g:draw_line(self.boids[i].x, self.boids[i].y, self.boids[j].x, self.boids[j].y, 1.0)
            end
        end
    end

    for i = 1, n do
        local b = self.boids[i]
        local r = b.radius
        if r > 3.2 then
            local outer_r = r * 2.5
            g:set_color(30, 65, 100, 0.35)
            g:fill_ellipse(b.x - outer_r, b.y - outer_r, outer_r * 2, outer_r * 2)
        end
        local mid_r = r * 1.5
        g:set_color(55, 110, 160, 0.5)
        g:fill_ellipse(b.x - mid_r, b.y - mid_r, mid_r * 2, mid_r * 2)
        local core_r = r * 0.8
        g:set_color(225, 240, 255, 0.95)
        g:fill_ellipse(b.x - core_r, b.y - core_r, core_r * 2, core_r * 2)
    end

    if self.is_mouse_down then
        g:set_color(80, 170, 250, 0.6)
        g:stroke_ellipse(self.mouse_x - 14, self.mouse_y - 14, 28, 28, 1.5)
        g:set_color(220, 245, 255, 0.85)
        g:fill_ellipse(self.mouse_x - 3, self.mouse_y - 3, 6, 6)
    end
end
