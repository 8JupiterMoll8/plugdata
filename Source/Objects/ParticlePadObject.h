/*
 // Copyright (c) 2026 Timothy Schoen & Jupiter Moll
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
 */
#pragma once

#include "ObjectBase.h"
#include "Utility/GlobalMouseListener.h"
#include "PluginProcessor.h"
#include "Canvas.h"
#include "PluginEditor.h"
#include <vector>
#include <cmath>
#include <random>

struct t_fake_particlepad {
    t_object x_obj;
    t_glist* x_glist;
    t_symbol* x_bindname;
    int x_w;
    int x_h;
    int x_count;
    float x_gravity_x;
    float x_gravity_y;
    float x_friction;
    unsigned char x_color[3];
    int x_zoom;
    int x_edit;
    t_outlet* x_out_pos;
    t_outlet* x_out_energy;
    t_outlet* x_out_bang;
};

class ParticlePadObject final : public ObjectBase
    , public Timer {

    struct Particle {
        float x = 0.0f;
        float y = 0.0f;
        float vx = 0.0f;
        float vy = 0.0f;
        float radius = 4.0f;
    };

    std::vector<Particle> particles;
    bool isAttracting = false;
    Point<float> attractorPos;
    Point<float> lastMousePos;
    Point<float> mouseVelocity;

    Value sizeProperty = SynchronousValue();
    NVGcolor fillColour;

    float friction = 0.97f;
    float gravityX = 0.0f;
    float gravityY = 0.0f;
    int particleCount = 24;

public:
    ParticlePadObject(pd::WeakReference ptr, Object* object)
        : ObjectBase(ptr, object)
        , mouseListener(this)
    {
        fillColour = nvgRGBA(24, 25, 32, 240);

        initParticles(particleCount);

        mouseListener.globalMouseDown = [this](MouseEvent const& e) {
            auto const relativeEvent = e.getEventRelativeTo(this);

            if (!getLocalBounds().contains(relativeEvent.getPosition()) || !isInsideGraphBounds(e) || !isLocked() || !cnv->isShowing() || cnv->editor->openedDialog)
                return;

            isAttracting = true;
            attractorPos = relativeEvent.getPosition().toFloat();
            lastMousePos = attractorPos;
            mouseVelocity = { 0.0f, 0.0f };
        };

        mouseListener.globalMouseUp = [this](MouseEvent const& e) {
            (void)e;
            if (isAttracting) {
                // Impart flick velocity to nearby particles on mouse release
                for (auto& p : particles) {
                    float const dx = p.x - attractorPos.x;
                    float const dy = p.y - attractorPos.y;
                    float const d = std::sqrt(dx * dx + dy * dy);
                    if (d < 60.0f && d > 0.001f) {
                        p.vx += mouseVelocity.x * 0.4f;
                        p.vy += mouseVelocity.y * 0.4f;
                    }
                }
            }
            isAttracting = false;
        };

        mouseListener.globalMouseMove = [this](MouseEvent const& e) {
            if (!isAttracting || !isInsideGraphBounds(e) || !isLocked() || !cnv->isShowing() || cnv->editor->openedDialog)
                return;

            auto const relativeEvent = e.getEventRelativeTo(this);
            Point<float> const currentPos = relativeEvent.getPosition().toFloat();
            mouseVelocity = currentPos - lastMousePos;
            lastMousePos = currentPos;
            attractorPos = currentPos;
        };

        mouseListener.globalMouseDrag = [this](MouseEvent const& e) {
            mouseListener.globalMouseMove(e);
        };

        setInterceptsMouseClicks(false, false);
        objectParameters.addParamSize(&sizeProperty);

        // Run simulation at 60 Hz
        startTimerHz(60);
    }

    ~ParticlePadObject() override
    {
        stopTimer();
    }

    void initParticles(int count)
    {
        particles.clear();
        int const w = std::max(getWidth(), 120);
        int const h = std::max(getHeight(), 120);

        std::mt19937 rng(1337);
        std::uniform_real_distribution<float> distX(20.0f, static_cast<float>(w - 20));
        std::uniform_real_distribution<float> distY(20.0f, static_cast<float>(h - 20));
        std::uniform_real_distribution<float> distV(-2.5f, 2.5f);

        for (int i = 0; i < count; ++i) {
            Particle p;
            p.x = distX(rng);
            p.y = distY(rng);
            p.vx = distV(rng);
            p.vy = distV(rng);
            p.radius = 3.5f + static_cast<float>(i % 3);
            particles.push_back(p);
        }
    }

    void timerCallback() override
    {
        if (particles.empty())
            return;

        int const w = getWidth();
        int const h = getHeight();
        if (w <= 10 || h <= 10)
            return;

        bool didCollide = false;
        float totalEnergy = 0.0f;
        float sumX = 0.0f;
        float sumY = 0.0f;

        // Sync parameters from Pd structure if available
        if (auto pad = ptr.get<t_fake_particlepad>()) {
            if (pad->x_count != particleCount && pad->x_count > 0 && pad->x_count <= 128) {
                particleCount = pad->x_count;
                initParticles(particleCount);
            }
            if (pad->x_friction > 0.5f && pad->x_friction <= 1.0f) {
                friction = pad->x_friction;
            }
            gravityX = pad->x_gravity_x;
            gravityY = pad->x_gravity_y;
        }

        // Particle physics update
        size_t const n = particles.size();
        for (size_t i = 0; i < n; ++i) {
            auto& p = particles[i];

            // Attraction to mouse cursor
            if (isAttracting) {
                float const dx = attractorPos.x - p.x;
                float const dy = attractorPos.y - p.y;
                float const distSq = dx * dx + dy * dy;
                float const dist = std::sqrt(distSq) + 5.0f;
                float const pull = std::min(4.5f, 80.0f / dist);
                p.vx += (dx / dist) * pull;
                p.vy += (dy / dist) * pull;
            }

            // Gravity & friction
            p.vx += gravityX;
            p.vy += gravityY;
            p.vx *= friction;
            p.vy *= friction;

            // Update position
            p.x += p.vx;
            p.y += p.vy;

            // Elastic bounce off borders
            if (p.x < p.radius) {
                p.x = p.radius;
                p.vx = -p.vx * 0.88f;
                didCollide = true;
            } else if (p.x > static_cast<float>(w) - p.radius) {
                p.x = static_cast<float>(w) - p.radius;
                p.vx = -p.vx * 0.88f;
                didCollide = true;
            }

            if (p.y < p.radius) {
                p.y = p.radius;
                p.vy = -p.vy * 0.88f;
                didCollide = true;
            } else if (p.y > static_cast<float>(h) - p.radius) {
                p.y = static_cast<float>(h) - p.radius;
                p.vy = -p.vy * 0.88f;
                didCollide = true;
            }

            // Inter-particle gentle separation
            for (size_t j = i + 1; j < n; ++j) {
                auto& p2 = particles[j];
                float const dx = p2.x - p.x;
                float const dy = p2.y - p.y;
                float const distSq = dx * dx + dy * dy;
                if (distSq < 144.0f && distSq > 0.0001f) {
                    float const d = std::sqrt(distSq);
                    float const push = (12.0f - d) * 0.15f;
                    float const nx = dx / d;
                    float const ny = dy / d;
                    p.vx -= nx * push;
                    p.vy -= ny * push;
                    p2.vx += nx * push;
                    p2.vy += ny * push;
                }
            }

            totalEnergy += (p.vx * p.vx + p.vy * p.vy);
            sumX += p.x;
            sumY += p.y;
        }

        // Send outlets safely on Pd audio thread
        float const normX = std::clamp(sumX / (static_cast<float>(n) * static_cast<float>(w)), 0.0f, 1.0f);
        float const normY = std::clamp(1.0f - (sumY / (static_cast<float>(n) * static_cast<float>(h))), 0.0f, 1.0f);
        float const avgEnergy = totalEnergy / static_cast<float>(n);
        float const normEnergy = std::clamp(avgEnergy / 25.0f, 0.0f, 1.0f);

        pd->enqueueFunctionAsync<t_fake_particlepad>(ptr, [normX, normY, normEnergy, didCollide](t_fake_particlepad* pad) {
            sys_lock();
            if (pad->x_out_pos) {
                StackArray<t_atom, 2> at;
                SETFLOAT(&at[0], normX);
                SETFLOAT(&at[1], normY);
                outlet_list(pad->x_out_pos, gensym("list"), 2, at.data());
            }

            if (pad->x_out_energy) {
                outlet_float(pad->x_out_energy, normEnergy);
            }

            if (didCollide && pad->x_out_bang) {
                outlet_bang(pad->x_out_bang);
            }
            sys_unlock();
        });

        repaint();
    }

    void render(NVGcontext* nvg) override
    {
        auto const b = getLocalBounds().toFloat();
        auto const outlineColour = object->isSelected() && !cnv->isGraph ? cnv->selectedOutlineCol : cnv->objectOutlineCol;

        // 1. Draw rounded background container
        nvgDrawRoundedRect(nvg, b.getX(), b.getY(), b.getWidth(), b.getHeight(), fillColour, outlineColour, Corners::objectCornerRadius);

        // 2. Draw proximity web/network lines between nearby particles
        size_t const n = particles.size();
        constexpr float maxDist = 48.0f;
        constexpr float maxDistSq = maxDist * maxDist;

        for (size_t i = 0; i < n; ++i) {
            for (size_t j = i + 1; j < n; ++j) {
                float const dx = particles[j].x - particles[i].x;
                float const dy = particles[j].y - particles[i].y;
                float const dSq = dx * dx + dy * dy;

                if (dSq < maxDistSq) {
                    float const alpha = (1.0f - (dSq / maxDistSq)) * 140.0f;
                    nvgBeginPath(nvg);
                    nvgMoveTo(nvg, particles[i].x, particles[i].y);
                    nvgLineTo(nvg, particles[j].x, particles[j].y);
                    nvgStrokeColor(nvg, nvgRGBA(70, 160, 240, static_cast<unsigned char>(alpha)));
                    nvgStrokeWidth(nvg, 1.0f);
                    nvgStroke(nvg);
                }
            }
        }

        // 3. Draw particles (outer halo + glowing core)
        for (auto const& p : particles) {
            float const speed = std::sqrt(p.vx * p.vx + p.vy * p.vy);
            float const speedRatio = std::clamp(speed / 8.0f, 0.0f, 1.0f);

            // Halo
            nvgBeginPath(nvg);
            nvgCircle(nvg, p.x, p.y, p.radius * 2.2f);
            unsigned char const haloR = static_cast<unsigned char>(60.0f + speedRatio * 180.0f);
            unsigned char const haloG = static_cast<unsigned char>(160.0f - speedRatio * 80.0f);
            unsigned char const haloB = static_cast<unsigned char>(255.0f - speedRatio * 150.0f);
            nvgFillColor(nvg, nvgRGBA(haloR, haloG, haloB, 45));
            nvgFill(nvg);

            // Core
            nvgBeginPath(nvg);
            nvgCircle(nvg, p.x, p.y, p.radius);
            nvgFillColor(nvg, nvgRGBA(230, 245, 255, 230));
            nvgFill(nvg);
        }

        // 4. Draw mouse attractor ripple when active
        if (isAttracting) {
            nvgBeginPath(nvg);
            nvgCircle(nvg, attractorPos.x, attractorPos.y, 14.0f);
            nvgStrokeColor(nvg, nvgRGBA(255, 190, 80, 180));
            nvgStrokeWidth(nvg, 1.5f);
            nvgStroke(nvg);
        }
    }

    bool isInsideGraphBounds(MouseEvent const& e) const
    {
        auto const* topLevel = cnv;
        while (auto const* nextCanvas = topLevel->findParentComponentOfClass<Canvas>()) {
            topLevel = nextCanvas;
            if (auto* graph = dynamic_cast<GraphOnParent*>(topLevel->getParentComponent())) {
                auto const pos = e.getEventRelativeTo(graph).getPosition();
                if (!graph->getLocalBounds().contains(pos)) {
                    return false;
                }
            }
        }
        return true;
    }

    bool isLocked() const
    {
        auto const* topLevel = cnv;
        while (auto const* nextCanvas = topLevel->findParentComponentOfClass<Canvas>()) {
            topLevel = nextCanvas;
        }
        return getValue<bool>(topLevel->locked) || getValue<bool>(topLevel->commandLocked) || topLevel->isGraph;
    }

    void setPdBounds(Rectangle<int> const b) override
    {
        if (auto pad = ptr.get<t_fake_particlepad>()) {
            auto* patch = cnv->patch.getRawPointer();
            pd::Interface::moveObject(patch, pad.cast<t_gobj>(), b.getX(), b.getY());
            pad->x_w = b.getWidth() - 1;
            pad->x_h = b.getHeight() - 1;
        }
    }

    Rectangle<int> getPdBounds() override
    {
        if (auto gobj = ptr.get<t_gobj>()) {
            auto* patch = cnv->patch.getRawPointer();
            int x = 0, y = 0, w = 0, h = 0;
            pd::Interface::getObjectBounds(patch, gobj.get(), &x, &y, &w, &h);
            return { x, y, w + 1, h + 1 };
        }
        return {};
    }

    void update() override
    {
        if (auto pad = ptr.get<t_fake_particlepad>()) {
            sizeProperty = VarArray { var(pad->x_w), var(pad->x_h) };
            fillColour = NVGComponent::convertColour(Colour(pad->x_color[0], pad->x_color[1], pad->x_color[2]));
            if (pad->x_count > 0 && pad->x_count != particleCount) {
                particleCount = pad->x_count;
                initParticles(particleCount);
            }
        }
    }

    void updateSizeProperty() override
    {
        setPdBounds(object->getObjectBounds());
        if (auto pad = ptr.get<t_fake_particlepad>()) {
            setParameterExcludingListener(sizeProperty, VarArray { var(pad->x_w), var(pad->x_h) });
        }
    }

    void propertyChanged(Value& value) override
    {
        if (value.refersToSameSourceAs(sizeProperty)) {
            auto const& arr = *sizeProperty.getValue().getArray();
            auto const* constrainer = getConstrainer();
            auto const width = std::max(static_cast<int>(arr[0]), constrainer->getMinimumWidth());
            auto const height = std::max(static_cast<int>(arr[1]), constrainer->getMinimumHeight());

            setParameterExcludingListener(sizeProperty, VarArray { var(width), var(height) });

            if (auto pad = ptr.get<t_fake_particlepad>()) {
                pad->x_w = width;
                pad->x_h = height;
            }

            object->updateBounds();
        }
    }

    GlobalMouseListener mouseListener;
};
