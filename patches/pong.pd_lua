-- pong.pd_lua - Sonic Pong Interactive Game Synthesizer for PlugData
-- Author: Antigravity & Jupiter Moll
-- Aesthetic: Cyberpunk Neon Vector Pong with Particle Bursts, Harmonic Bricks & Procedural Audio Telemetry

local pong = pd.Class:new():register("pong")

function pong:initialize(sel, atoms)
    -- Inlets:
    -- 1: Reset / Ball Speed (2.0 - 12.0)
    -- 2: Paddle Width (30 - 120)
    self.inlets = 2

    -- Outlets:
    -- 1: Paddle Hit: list (midi_pitch, speed)
    -- 2: Brick Hit: list (midi_pitch, brick_idx)
    -- 3: Wall Bounce: bang
    -- 4: Continuous Telemetry: list (norm_x, norm_y, speed)
    -- 5: Miss / Reset: bang
    self.outlets = 5

    self.width = 280
    self.height = 360
    self:set_size(self.width, self.height)

    self.paddle_w = 64
    self.paddle_h = 10
    self.paddle_x = (self.width - self.paddle_w) / 2
    self.paddle_y = self.height - 24

    self.ball_r = 5
    self.base_speed = 5.2
    self.max_speed = 14.0

    self.score = 0
    self.combo = 0
    self.high_score = 0

    self.particles = {}
    self.trail = {}

    -- Musical scale for bricks (C Dorian / Minor Pentatonic):
    -- C4, D4, Eb4, F4, G4, Bb4, C5, D5, Eb5, F5, G5, Bb5
    self.brick_scale = { 60, 62, 63, 65, 67, 70, 72, 74, 75, 77, 79, 82 }
    self.bricks = {}
    self:reset_bricks()
    self:reset_ball(1)

    return true
end

function pong:postinitialize()
    self:set_size(self.width, self.height)
    self.clock = pd.Clock:new():register(self, "tick")
    self.clock:delay(10) -- 100 FPS
end

function pong:finalize()
    if self.clock then
        self.clock:destruct()
    end
end

function pong:reset_bricks()
    self.bricks = {}
    local rows = 3
    local cols = 5
    local bw = (self.width - 24) / cols
    local bh = 14
    for r = 1, rows do
        for c = 1, cols do
            local idx = (r - 1) * cols + c
            local pitch = self.brick_scale[((idx - 1) % #self.brick_scale) + 1]
            table.insert(self.bricks, {
                x = 12 + (c - 1) * bw + 2,
                y = 36 + (r - 1) * (bh + 4),
                w = bw - 4,
                h = bh,
                alive = true,
                pitch = pitch,
                color_r = 60 + r * 45,
                color_g = 140 + c * 20,
                color_b = 230 - r * 30
            })
        end
    end
end

function pong:reset_ball(dir_y)
    self.ball_x = self.width / 2
    self.ball_y = self.height / 2 + 20
    local angle = (math.random() * 0.8 - 0.4) + (dir_y > 0 and 1.57 or -1.57)
    local spd = self.base_speed
    self.ball_vx = math.cos(angle) * spd
    self.ball_vy = math.sin(angle) * spd
    self.trail = {}
    self.combo = 0
end

function pong:spawn_burst(x, y, count, r, g, b)
    for i = 1, count do
        local a = math.random() * 6.28
        local spd = 1.0 + math.random() * 4.5
        table.insert(self.particles, {
            x = x,
            y = y,
            vx = math.cos(a) * spd,
            vy = math.sin(a) * spd,
            life = 1.0,
            decay = 0.035 + math.random() * 0.035,
            size = 2.0 + math.random() * 2.5,
            r = r or 100,
            g = g or 200,
            b = b or 255
        })
    end
end

-- Inlets
function pong:in_1_float(f)
    self.base_speed = math.max(2.0, math.min(12.0, f))
end

function pong:in_1_bang()
    self.score = 0
    self:reset_bricks()
    self:reset_ball(-1)
end

function pong:in_2_float(f)
    self.paddle_w = math.max(30, math.min(140, f))
end

-- Mouse Drag / Move controls paddle
function pong:mouse_drag(x, y)
    self.paddle_x = math.max(4, math.min(self.width - self.paddle_w - 4, x - self.paddle_w / 2))
end

function pong:mouse_move(x, y)
    self.paddle_x = math.max(4, math.min(self.width - self.paddle_w - 4, x - self.paddle_w / 2))
end

function pong:mouse_down(x, y)
    self.paddle_x = math.max(4, math.min(self.width - self.paddle_w - 4, x - self.paddle_w / 2))
    -- Click can also launch / unpause
    if math.abs(self.ball_vx) < 0.1 and math.abs(self.ball_vy) < 0.1 then
        self:reset_ball(-1)
    end
end

-- Game Physics Loop (100 FPS)
function pong:tick()
    if not self.has_resized then
        self:set_size(self.width, self.height)
        self.has_resized = true
    end

    -- 1. Update Ball Trail
    table.insert(self.trail, 1, { x = self.ball_x, y = self.ball_y })
    if #self.trail > 8 then
        table.remove(self.trail)
    end

    -- 2. Move Ball
    self.ball_x = self.ball_x + self.ball_vx
    self.ball_y = self.ball_y + self.ball_vy

    -- 3. Wall Collisions (Left / Right)
    if self.ball_x - self.ball_r < 6 then
        self.ball_x = 6 + self.ball_r
        self.ball_vx = -self.ball_vx
        self:spawn_burst(self.ball_x, self.ball_y, 6, 80, 160, 240)
        self:outlet(3, "bang", {})
    elseif self.ball_x + self.ball_r > self.width - 6 then
        self.ball_x = self.width - 6 - self.ball_r
        self.ball_vx = -self.ball_vx
        self:spawn_burst(self.ball_x, self.ball_y, 6, 80, 160, 240)
        self:outlet(3, "bang", {})
    end

    -- 4. Top Wall Collision
    if self.ball_y - self.ball_r < 6 then
        self.ball_y = 6 + self.ball_r
        self.ball_vy = -self.ball_vy
        self:spawn_burst(self.ball_x, self.ball_y, 8, 120, 220, 255)
        self:outlet(3, "bang", {})
    end

    -- 5. Paddle Collision (Player)
    if self.ball_vy > 0
       and self.ball_y + self.ball_r >= self.paddle_y
       and self.ball_y - self.ball_r <= self.paddle_y + self.paddle_h
       and self.ball_x >= self.paddle_x - 4
       and self.ball_x <= self.paddle_x + self.paddle_w + 4 then

        -- Where on paddle was it hit? (-1.0 to 1.0)
        local hit_norm = (self.ball_x - (self.paddle_x + self.paddle_w / 2)) / (self.paddle_w / 2)
        hit_norm = math.max(-1.0, math.min(1.0, hit_norm))

        -- Reflection angle
        local bounce_angle = hit_norm * 1.05 -- up to ~60 deg
        local spd = math.min(self.max_speed, math.sqrt(self.ball_vx * self.ball_vx + self.ball_vy * self.ball_vy) * 1.04)

        self.ball_vx = math.sin(bounce_angle) * spd
        self.ball_vy = -math.abs(math.cos(bounce_angle) * spd)
        self.ball_y = self.paddle_y - self.ball_r - 1

        self.score = self.score + 10
        self.combo = self.combo + 1
        if self.score > self.high_score then self.high_score = self.score end

        self:spawn_burst(self.ball_x, self.paddle_y, 14, 255, 200, 80)

        -- Musical Paddle Pitch: 48 (C3) at left to 60 (C4) at right
        local paddle_pitch = 48 + math.floor((hit_norm + 1.0) * 6.0 + 0.5)
        self:outlet(1, "list", { paddle_pitch, spd })
    end

    -- 6. Brick Collisions
    local all_cleared = true
    for i = 1, #self.bricks do
        local b = self.bricks[i]
        if b.alive then
            all_cleared = false
            if self.ball_x + self.ball_r >= b.x
               and self.ball_x - self.ball_r <= b.x + b.w
               and self.ball_y + self.ball_r >= b.y
               and self.ball_y - self.ball_r <= b.y + b.h then

                b.alive = false
                self.ball_vy = -self.ball_vy
                self.score = self.score + 50 * math.max(1, self.combo)
                if self.score > self.high_score then self.high_score = self.score end

                self:spawn_burst(b.x + b.w / 2, b.y + b.h / 2, 18, b.color_r, b.color_g, b.color_b)
                self:outlet(2, "list", { b.pitch, i })
                break
            end
        end
    end

    -- Auto-respawn bricks if cleared
    if all_cleared then
        self:reset_bricks()
        self:spawn_burst(self.width / 2, 60, 30, 255, 230, 100)
    end

    -- 7. Bottom Miss
    if self.ball_y - self.ball_r > self.height then
        self:spawn_burst(self.ball_x, self.height - 4, 25, 255, 60, 60)
        self:outlet(5, "bang", {})
        self:reset_ball(-1)
    end

    -- 8. Update Particles
    for i = #self.particles, 1, -1 do
        local p = self.particles[i]
        p.x = p.x + p.vx
        p.y = p.y + p.vy
        p.vx = p.vx * 0.96
        p.vy = p.vy * 0.96
        p.life = p.life - p.decay
        if p.life <= 0 then
            table.remove(self.particles, i)
        end
    end

    -- 9. Stream Continuous Telemetry
    local norm_x = math.max(0.0, math.min(1.0, self.ball_x / self.width))
    local norm_y = math.max(0.0, math.min(1.0, 1.0 - (self.ball_y / self.height)))
    local cur_spd = math.sqrt(self.ball_vx * self.ball_vx + self.ball_vy * self.ball_vy)
    self:outlet(4, "list", { norm_x, norm_y, cur_spd })

    self:repaint()
    self.clock:delay(10)
end

-- NanoVG Render Callback
function pong:paint(g)
    -- Sleek Dark Cyberpunk Backdrop
    g:set_color(16, 18, 24, 1.0)
    g:fill_rounded_rect(0, 0, self.width, self.height, 8)

    -- Border
    g:set_color(40, 48, 65, 0.9)
    g:stroke_rounded_rect(0, 0, self.width, self.height, 8, 1.0)

    -- Center subtle guideline
    g:set_color(30, 36, 50, 0.4)
    g:draw_line(10, self.height / 2, self.width - 10, self.height / 2, 1.0)

    -- HUD Score Text
    g:set_color(160, 180, 210, 0.75)
    g:draw_text(string.format("SCORE: %d", self.score), 14, 22, 100, 14, 1)
    g:set_color(100, 220, 180, 0.75)
    g:draw_text(string.format("COMBO: x%d", self.combo), self.width - 110, 22, 96, 14, 2)

    -- Draw Bricks
    for i = 1, #self.bricks do
        local b = self.bricks[i]
        if b.alive then
            -- Glow halo
            g:set_color(b.color_r, b.color_g, b.color_b, 0.30)
            g:fill_rounded_rect(b.x - 2, b.y - 2, b.w + 4, b.h + 4, 4)
            -- Solid core
            g:set_color(b.color_r, b.color_g, b.color_b, 0.90)
            g:fill_rounded_rect(b.x, b.y, b.w, b.h, 3)
            -- Top highlight
            g:set_color(255, 255, 255, 0.45)
            g:draw_line(b.x + 2, b.y + 2, b.x + b.w - 2, b.y + 2, 1.0)
        end
    end

    -- Draw Particles
    for i = 1, #self.particles do
        local p = self.particles[i]
        local alpha = math.max(0.0, p.life)
        g:set_color(p.r, p.g, p.b, alpha)
        g:fill_ellipse(p.x - p.size / 2, p.y - p.size / 2, p.size, p.size)
    end

    -- Draw Ball Motion Trail
    for i = 1, #self.trail do
        local t = self.trail[i]
        local a = (1.0 - (i / #self.trail)) * 0.35
        local sz = self.ball_r * 2 * (1.0 - i * 0.08)
        g:set_color(100, 200, 255, a)
        g:fill_ellipse(t.x - sz / 2, t.y - sz / 2, sz, sz)
    end

    -- Draw Ball
    -- Ambient glow
    g:set_color(80, 180, 255, 0.45)
    g:fill_ellipse(self.ball_x - self.ball_r * 2, self.ball_y - self.ball_r * 2, self.ball_r * 4, self.ball_r * 4)
    -- Bright Core
    g:set_color(235, 245, 255, 1.0)
    g:fill_ellipse(self.ball_x - self.ball_r, self.ball_y - self.ball_r, self.ball_r * 2, self.ball_r * 2)

    -- Draw Player Paddle
    -- Halo glow
    g:set_color(60, 180, 255, 0.35)
    g:fill_rounded_rect(self.paddle_x - 3, self.paddle_y - 3, self.paddle_w + 6, self.paddle_h + 6, 6)
    -- Paddle body
    g:set_color(40, 150, 240, 0.95)
    g:fill_rounded_rect(self.paddle_x, self.paddle_y, self.paddle_w, self.paddle_h, 4)
    -- Neon core line
    g:set_color(210, 245, 255, 0.90)
    g:draw_line(self.paddle_x + 4, self.paddle_y + self.paddle_h / 2, self.paddle_x + self.paddle_w - 4, self.paddle_y + self.paddle_h / 2, 2.0)
end
