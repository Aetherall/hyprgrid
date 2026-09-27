// Kinetics: iOS-style release for the view (View.cpp). Motion.hpp decides
// where a release lands (velocity tracking + UIKit projection); Hyprland
// animates it, on one of its own spring curves, launched at the fingers'
// speed. Hyprland's animation manager then drives the frames and fires end
// callbacks when it settles.
//
// The base spring is a curve named "kinetic" in the Hyprland config:
//   hl.curve("kinetic", { type = "spring", stiffness = w^2, damping = 2w, mass = 1 })
// Keep it critically damped (damping = 2 sqrt(stiffness * mass)): overshoot
// would show the far side of whatever is released. Each launch copies it,
// stiffened when the fingers are faster than it can absorb (see launch()).
//
// INCLUDE THIS FIRST in the translation unit: it opens up hyprutils' animated
// variable (the launch velocity is private) before any Hyprland header pulls
// it in, pulling in that header's own dependencies first so their include
// guards keep them out of the #define.

#pragma once

#include <chrono>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include <hyprutils/memory/WeakPtr.hpp>
#include <hyprutils/memory/SharedPtr.hpp>
#include <hyprutils/signal/Signal.hpp>
#include <hyprutils/math/Vector2D.hpp>
#include <hyprutils/animation/BezierCurve.hpp>
#include <hyprutils/animation/AnimationConfig.hpp>
#include <hyprutils/animation/AnimationManager.hpp>
#define private public
#include <hyprutils/animation/AnimatedVariable.hpp>
#undef private

#include <hyprland/src/helpers/AnimatedVariable.hpp>
#include <hyprland/src/animation/AnimationManager.hpp>

#include <algorithm>

namespace Kinetics {
    inline constexpr const char* BASE_SPRING = "kinetic";
    // Without the Lua declaration: a 0.3s critically damped spring.
    inline constexpr float DEFAULT_OMEGA = 2 * M_PI / 0.3;
    // Cap on the flick stiffening (a 50ms period).
    inline constexpr float MAX_OMEGA = 2 * M_PI / 0.05;

    inline float baseOmega() {
        const auto SPRING = Animation::mgr()->getSpring(BASE_SPRING);
        if (!SPRING)
            return DEFAULT_OMEGA;
        return std::sqrt(std::max(SPRING->stiffness, 0.0001F) / std::max(SPRING->mass, 0.0001F));
    }

    // One animation config per launch name, pointing at that name's spring.
    // Configs must outlive the variables using them, hence static.
    inline Hyprutils::Memory::CSharedPointer<Hyprutils::Animation::SAnimationPropertyConfig> configFor(const std::string& name) {
        static std::unordered_map<std::string, Hyprutils::Memory::CSharedPointer<Hyprutils::Animation::SAnimationPropertyConfig>> configs;
        auto& cfg = configs[name];
        if (!cfg) {
            cfg                  = Hyprutils::Memory::makeShared<Hyprutils::Animation::SAnimationPropertyConfig>();
            cfg->overridden      = true;
            cfg->internalBezier  = "spring:" + name;
            cfg->internalSpeed   = 1.F;
            cfg->internalEnabled = 1;
            cfg->pValues         = cfg;
        }
        return cfg;
    }

    // Finish `var`'s current animation (toward its goal, from where it is now)
    // on the spring `name`, launched at `progressVelocity`: the fraction of the
    // remaining distance covered per second, > 0 toward the goal. Critically
    // damped from rest it never overshoots; launched toward the goal faster
    // than w it would, so w is raised to match: it then decays exponentially
    // from exactly that speed. The var keeps its style; it goes back to its
    // own config the next time its owner sets one.
    template <typename T>
    void launch(const PHLANIMVAR<T>& var, const std::string& name, float progressVelocity) {
        if (!var || !var->isBeingAnimated() || var->value() == var->goal())
            return;

        float omega = baseOmega();
        if (progressVelocity > omega)
            omega = std::min(progressVelocity, MAX_OMEGA);

        Hyprutils::Animation::SSpringCurve curve;
        curve.stiffness       = omega * omega;
        curve.damping         = 2 * omega;
        curve.mass            = 1.F;
        curve.valueEpsilon    = 0.001F;
        curve.velocityEpsilon = omega * 0.002F;
        Animation::mgr()->addSpringWithName(name, curve);

        const auto CFG      = configFor(name);
        const auto PREVIOUS = var->getConfig().lock();
        CFG->internalStyle  = PREVIOUS && PREVIOUS->pValues ? PREVIOUS->pValues->internalStyle : "";
        var->setConfig(CFG);

        // Restart the curve from here: progress 0 at the current value.
        var->m_Begun           = var->value();
        var->m_fSpringValue    = 0.F;
        var->m_fSpringVelocity = progressVelocity;
        var->springLastStep    = std::chrono::steady_clock::now();
    }

    // Current velocity (value units/s) of a var moving on a launched spring;
    // zero when it isn't, e.g. to carry momentum into a retarget. (A template,
    // like launch(), so files that include this late never compile the
    // private access.)
    template <typename T>
    T velocityOf(const PHLANIMVAR<T>& var) {
        if (!var || !var->isBeingAnimated() || !var->isSpringCurve())
            return T{};
        return (var->goal() - var->m_Begun) * var->m_fSpringVelocity;
    }

    template <typename T>
    bool isLaunched(const PHLANIMVAR<T>& var, const std::string& name) {
        return var && var->isBeingAnimated() && var->getConfig().lock() == configFor(name);
    }

    // For owners that never set their var's config again: call from the var's
    // update callback to hand it back its own config once a launch settles
    // (or is warped away), so its other animations keep their usual curve.
    template <typename T>
    void restoreWhenSettled(const PHLANIMVAR<T>& var, const Hyprutils::Memory::CSharedPointer<Hyprutils::Animation::SAnimationPropertyConfig>& own) {
        if (!var || !own || var->value() != var->goal())
            return;
        const auto CURRENT = var->getConfig().lock();
        if (CURRENT && CURRENT != own && CURRENT->internalBezier.starts_with("spring:kinetic"))
            var->setConfig(own);
    }
}
