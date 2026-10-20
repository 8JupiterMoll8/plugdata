-- flock.pd_lua - Nature of Code Boids Generative Flocking in PlugData via pdlua_gfx
-- Author: Antigravity & Jupiter Moll
-- Aesthetic: High-speed constellation boids, tiered scales, luminous depth halos, dynamic constellation webs

local flock = pd.Class:new():register("flock")

function flock:initialize(sel, atoms)
    -- 4 inlets for direct live slider control:
    -- Inlet 1: Speed (0.5 - 30.0) or selector messages / bang
    -- Inlet 2: Spread / Separation (0.5 - 25.0)
    -- Inlet 3: Gravity / Cohesion (0.001 - 0.25)
    -- Inlet 4: FPS / Refresh rate (20 - 144)
    self.inlets = 4
    self.outlets = 3 -- 1: list (centroid x y), 2: float (flock speed/energy), 3: bang (mouse trigger)

    self.width = 240
    self.height = 240
    self:set_size(self.width, self.height)

    self.num_boids = 26
    self.boids = {}
    self.max_speed = 5.6   -- Fast, energetic particle speed (was 2.8)
    self.max_force = 0.28   -- Snappy acceleration
    self.sep_force = 3.4
    self.coh_force = 0.02
    self.frame_delay = 10   -- 10ms = 100 FPS high-refresh rate (was 16ms = 60 FPS)

    for i = 1, self.num_boids do
        -- Tiered depth: 15% anchor nodes (radius ~7), 35% mid (radius ~4.8), 50% small (radius ~2.8)
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

    self.mouse_x = 120
    self.mouse_y = 120
    self.is_mouse_down = false

    return true
end

function flock:postinitialize()
    -- High-refresh update clock (default 10ms = 100 FPS)
    self.clock = pd.Clock:new():register(self, "tick")
    self.clock:delay(self.frame_delay or 10)
end

function flock:finalize()
    if self.clock then
        self.clock:destruct()
    end
end

-- Parameter Setters
function flock:set_speed(new_speed)
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

function flock:set_sep(val)
    local v = type(val) == "table" and val[1] or val
    v = tonumber(v)
    if not v then return end
    if v > 25.0 then v = v * 0.2 end
    self.sep_force = math.max(0.2, math.min(25.0, v))
end

function flock:set_coh(val)
    local v = type(val) == "table" and val[1] or val
    v = tonumber(v)
    if not v then return end
    if v > 1.0 then v = v * 0.001 end
    self.coh_force = math.max(0.001, math.min(0.25, v))
end

function flock:set_fps(val)
    local v = type(val) == "table" and val[1] or val
    v = tonumber(v)
    if not v then return end
    local target_fps = math.max(20, math.min(144, v))
    self.frame_delay = math.max(6, math.floor(1000 / target_fps))
end

-- Inlet 1 Handlers: Speed, Named Messages, Bang
function flock:in_1_float(f)
    self:set_speed(f)
end

function flock:in_1_speed(atoms)
    self:set_speed(atoms)
end

function flock:in_1_sep(atoms)
    self:set_sep(atoms)
end

function flock:in_1_coh(atoms)
    self:set_coh(atoms)
end

function flock:in_1_fps(atoms)
    self:set_fps(atoms)
end

function flock:in_1_bang()
    -- Burst scatter boids
    for i = 1, #self.boids do
        local angle = math.random() * 6.28
        local pwr = 3.0 + math.random() * self.max_speed
        self.boids[i].vx = math.cos(angle) * pwr
        self.boids[i].vy = math.sin(angle) * pwr
    end
end

function flock:in_1(sel, atoms)
    if sel == "speed" then
        self:set_speed(atoms)
    elseif sel == "sep" then
        self:set_sep(atoms)
    elseif sel == "coh" then
        self:set_coh(atoms)
    elseif sel == "fps" then
        self:set_fps(atoms)
    elseif sel == "bang" then
        self:in_1_bang()
    end
end

-- Inlet 2 Handlers: Spread / Separation
function flock:in_2_float(f)
    self:set_sep(f)
end

function flock:in_2_sep(atoms)
    self:set_sep(atoms)
end

function flock:in_2(sel, atoms)
    local v = sel == "sep" and atoms or sel
    self:set_sep(v)
end

-- Inlet 3 Handlers: Gravity / Cohesion
function flock:in_3_float(f)
    self:set_coh(f)
end

function flock:in_3_coh(atoms)
    self:set_coh(atoms)
end

function flock:in_3(sel, atoms)
    local v = sel == "coh" and atoms or sel
    self:set_coh(v)
end

-- Inlet 4 Handlers: FPS
function flock:in_4_float(f)
    self:set_fps(f)
end

function flock:in_4_fps(atoms)
    self:set_fps(atoms)
end

function flock:in_4(sel, atoms)
    local v = sel == "fps" and atoms or sel
    self:set_fps(v)
end

-- Mouse Events
function flock:mouse_down(x, y)
    self.is_mouse_down = true
    self.mouse_x = x
    self.mouse_y = y
    self:outlet(3, "bang", {})
end

function flock:mouse_drag(x, y)
    self.mouse_x = x
    self.mouse_y = y
end

function flock:mouse_up(x, y)
    self.is_mouse_down = false
end

-- Physics Loop (High-Refresh 100 FPS)
function flock:tick()
    local sum_x = 0
    local sum_y = 0
    local sum_vx = 0
    local sum_vy = 0
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

                -- 1. Separation (generous 42px buffer to prevent clumping)
                if dist_sq > 0 and dist_sq < 1764 then
                    local dist = math.sqrt(dist_sq)
                    sep_x = sep_x + (dx / dist) / dist
                    sep_y = sep_y + (dy / dist) / dist
                    sep_count = sep_count + 1
                end

                -- 2. Alignment & Cohesion (gentle 70px envelope)
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

        -- Apply Separation (strong repulsive force)
        if sep_count > 0 then
            local sf = self.sep_force or 3.4
            ax = ax + sep_x * sf
            ay = ay + sep_y * sf
        end

        -- Apply Alignment (smooth velocity matching)
        if ali_count > 0 then
            ali_x = ali_x / ali_count
            ali_y = ali_y / ali_count
            ax = ax + (ali_x - b1.vx) * 0.08
            ay = ay + (ali_y - b1.vy) * 0.08
        end

        -- Apply Cohesion (soft, loose attraction)
        if coh_count > 0 then
            coh_x = coh_x / coh_count
            coh_y = coh_y / coh_count
            local cf = self.coh_force or 0.02
            ax = ax + (coh_x - b1.x) * cf
            ay = ay + (coh_y - b1.y) * cf
        end

        -- Subtle autonomous wander drift
        b1.wander = b1.wander + 0.12
        ax = ax + math.cos(b1.wander) * 0.10
        ay = ay + math.sin(b1.wander) * 0.10

        -- Mouse Gravity Vortex Attractor (with celestial swirl momentum)
        if self.is_mouse_down then
            local mdx = self.mouse_x - b1.x
            local mdy = self.mouse_y - b1.y
            local mdist = math.sqrt(mdx * mdx + mdy * mdy) + 8
            local mforce = math.min(5.0, 120 / mdist)
            ax = ax + (mdx / mdist) * mforce - (mdy / mdist) * (mforce * 0.35)
            ay = ay + (mdy / mdist) * mforce + (mdx / mdist) * (mforce * 0.35)
        end

        -- Update velocity
        b1.vx = b1.vx + ax
        b1.vy = b1.vy + ay

        -- Responsive cruise propulsion: smoothly steer speed toward self.max_speed
        local spd = math.sqrt(b1.vx * b1.vx + b1.vy * b1.vy)
        if spd > 0.001 then
            local target_spd = self.max_speed
            local new_spd = spd + (target_spd - spd) * 0.20
            b1.vx = (b1.vx / spd) * new_spd
            b1.vy = (b1.vy / spd) * new_spd
            spd = new_spd
        else
            local angle = math.random() * 6.28
            b1.vx = math.cos(angle) * self.max_speed
            b1.vy = math.sin(angle) * self.max_speed
            spd = self.max_speed
        end

        -- Update position
        b1.x = b1.x + b1.vx
        b1.y = b1.y + b1.vy

        -- Soft organic boundary repulsion
        if b1.x < 22 then b1.vx = b1.vx + 0.4 end
        if b1.x > self.width - 22 then b1.vx = b1.vx - 0.4 end
        if b1.y < 22 then b1.vy = b1.vy + 0.4 end
        if b1.y > self.height - 22 then b1.vy = b1.vy - 0.4 end

        -- Hard bounce clamp
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

    -- Swarm dispersion (average distance from centroid)
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

    -- Flock alignment coherence (directional uniformity vs turbulence)
    local net_v = math.sqrt(sum_vx * sum_vx + sum_vy * sum_vy)
    local coherence = total_speed > 0.001 and (net_v / total_speed) or 1.0
    local chaos = math.max(0.0, math.min(1.0, 1.0 - coherence))

    -- Stream Telemetry to Outlets
    local norm_x = math.max(0.0, math.min(1.0, cx / self.width))
    local norm_y = math.max(0.0, math.min(1.0, 1.0 - (cy / self.height)))
    local avg_speed = math.max(0.0, math.min(1.0, (total_speed / n) / self.max_speed))

    self:outlet(1, "list", { norm_x, norm_y, dispersion, chaos })
    self:outlet(2, "float", { avg_speed })

    -- Trigger Hardware-Accelerated Repaint
    self:repaint()

    -- Next frame (high refresh rate)
    self.clock:delay(self.frame_delay or 10)
end

-- NanoVG Render Callback
function flock:paint(g)
    -- Dark sleek container
    g:set_color(22, 24, 29, 1.0)
    g:fill_rounded_rect(0, 0, self.width, self.height, 8)

    -- Subtle border
    g:set_color(45, 52, 68, 0.8)
    g:stroke_rounded_rect(0, 0, self.width, self.height, 8, 1.0)

    local n = #self.boids

    -- Draw subtle constellation web lines between proximate boids
    for i = 1, n do
        for j = i + 1, n do
            local dx = self.boids[j].x - self.boids[i].x
            local dy = self.boids[j].y - self.boids[i].y
            local dist_sq = dx * dx + dy * dy
            if dist_sq < 3025 then -- 55 px max connection distance
                local d = math.sqrt(dist_sq)
                local alpha = (1.0 - (d / 55.0)) * 0.45
                g:set_color(50, 100, 150, alpha)
                g:draw_line(self.boids[i].x, self.boids[i].y, self.boids[j].x, self.boids[j].y, 1.0)
            end
        end
    end

    -- Draw constellation nodes with multi-layered glow halos
    for i = 1, n do
        local b = self.boids[i]
        local r = b.radius

        -- Outer ambient halo (diffuse translucent glow for anchor & mid tiers)
        if r > 3.2 then
            local outer_r = r * 2.5
            g:set_color(30, 65, 100, 0.35)
            g:fill_ellipse(b.x - outer_r, b.y - outer_r, outer_r * 2, outer_r * 2)
        end

        -- Mid halo ring (richer cyan-blue)
        local mid_r = r * 1.5
        g:set_color(55, 110, 160, 0.5)
        g:fill_ellipse(b.x - mid_r, b.y - mid_r, mid_r * 2, mid_r * 2)

        -- Luminous inner core (ice white/cyan)
        local core_r = r * 0.8
        g:set_color(225, 240, 255, 0.95)
        g:fill_ellipse(b.x - core_r, b.y - core_r, core_r * 2, core_r * 2)
    end

    -- Draw mouse attractor vortex
    if self.is_mouse_down then
        g:set_color(80, 170, 250, 0.6)
        g:stroke_ellipse(self.mouse_x - 14, self.mouse_y - 14, 28, 28, 1.5)
        g:set_color(220, 245, 255, 0.85)
        g:fill_ellipse(self.mouse_x - 3, self.mouse_y - 3, 6, 6)
    end
end
