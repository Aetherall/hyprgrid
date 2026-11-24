// Floating windows spanning two monitors. A floating window belongs to the
// workspace of one monitor (its owner) but may lie partly on another; Hyprland
// draws that part there too. The owner's view moves on its own (View.cpp), so
// while it moves, that part would slide over the other monitor's content, or
// vanish at once when its workspace leaves. Instead, that part is shown only
// while the owner shows the window's workspace at rest: it fades out where it
// lies when the owner's view starts moving (a glide, a hold, the overview),
// and fades back in once the view settles on that workspace. On the owner's
// monitor, nothing changes.

#include "Span.hpp"
#include "Grid.hpp"
#include "View.hpp"
#include "overview/Overview.hpp"

#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/WindowRenderPresentation.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/workspace/HLWorkspace.hpp>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <unordered_map>

namespace {
    // Fade duration, ms, from fully shown to hidden or back.
    constexpr float FADE_MS = 150.F;

    typedef bool (*origShouldRenderWindow)(void*, PHLWINDOW, PHLMONITOR);
    typedef void (*origRenderWindow)(void*, Render::CRenderContext&, PHLWINDOW, PHLMONITOR, const Render::SWindowRenderPresentation&, const Time::steady_tp&, bool,
                                     Render::eRenderPassMode, bool, bool);

    CFunctionHook* g_shouldRenderHook = nullptr;
    CFunctionHook* g_renderWindowHook = nullptr;

    struct SFade {
        PHLWINDOWREF                          window;
        float                                 alpha = 0.F;
        std::chrono::steady_clock::time_point last;
    };
    std::unordered_map<const Desktop::View::CWindow*, SFade> g_fades;

    // The monitor owning `window`, when it is a floating window of a grid
    // workspace lying partly on `monitor`, another monitor; else null.
    PHLMONITOR spanOwner(const PHLWINDOW& window, const PHLMONITOR& monitor) {
        if (!window || !monitor || !window->isFloating() || (window->m_state & Desktop::View::WINDOW_STATE_PINNED))
            return nullptr;
        const auto WS = window->m_workspace;
        if (!WS || WS->type() == Workspace::eWorkspaceType::SPECIAL || !Grid::cellOf(WS))
            return nullptr;
        const auto OWNER = WS->m_monitor.lock();
        if (!OWNER || OWNER == monitor)
            return nullptr;
        if (window->getFullWindowBoundingBox().intersection(monitor->logicalBox()).empty())
            return nullptr;
        return OWNER;
    }

    // Shown: its workspace is the one the owner shows, at rest.
    float targetOf(const PHLWINDOW& window, const PHLMONITOR& owner) {
        return window->m_workspace == owner->m_activeWorkspace && !View::moving(owner) && !Overview::drawing(owner) ? 1.F : 0.F;
    }

    // The fade of `window`'s part outside its owner, moved toward `target`
    // by the time since it was last asked. A window first seen starts there.
    float fadeOf(const PHLWINDOW& window, float target) {
        const auto NOW  = std::chrono::steady_clock::now();
        auto&      fade = g_fades[window.get()];
        if (fade.window.lock() != window) {
            fade = {.window = window, .alpha = target, .last = NOW};
            return target;
        }
        const float STEP = std::chrono::duration<float, std::milli>(NOW - fade.last).count() / FADE_MS;
        fade.last        = NOW;
        fade.alpha       = fade.alpha < target ? std::min(target, fade.alpha + STEP) : std::max(target, fade.alpha - STEP);
        return fade.alpha;
    }

    void damage(const PHLWINDOW& window) {
        g_pHyprRenderer->damageBox(window->getFullWindowBoundingBox());
    }

    bool hkShouldRenderWindow(void* thisptr, PHLWINDOW window, PHLMONITOR monitor) {
        const bool ORIGINAL = rc<origShouldRenderWindow>(g_shouldRenderHook->m_original)(thisptr, window, monitor);
        const auto OWNER    = spanOwner(window, monitor);
        if (!OWNER)
            return ORIGINAL;
        const float TARGET = targetOf(window, OWNER);
        const float ALPHA  = fadeOf(window, TARGET);
        if (ALPHA != TARGET)
            damage(window); // fading: draw again
        return ALPHA > 0.F;
    }

    void hkRenderWindow(void* thisptr, Render::CRenderContext& ctx, PHLWINDOW window, PHLMONITOR monitor, const Render::SWindowRenderPresentation& presentation,
                        const Time::steady_tp& time, bool decorate, Render::eRenderPassMode mode, bool ignorePosition, bool standalone) {
        const auto ORIGINAL = rc<origRenderWindow>(g_renderWindowHook->m_original);
        const auto OWNER    = ignorePosition || standalone || presentation.workspaceScene ? nullptr : spanOwner(window, monitor);
        const auto FADE     = OWNER ? g_fades.find(window.get()) : g_fades.end();
        if (FADE == g_fades.end() || FADE->second.window.lock() != window) {
            ORIGINAL(thisptr, ctx, window, monitor, presentation, time, decorate, mode, ignorePosition, standalone);
            return;
        }
        if (FADE->second.alpha <= 0.F)
            return;
        // Where it lies at rest, whatever the owner's view is doing.
        auto shown            = presentation;
        shown.workspaceOffset = {};
        shown.fadeAlpha *= FADE->second.alpha;
        shown.alphaVisible = true;
        ORIGINAL(thisptr, ctx, window, monitor, shown, time, decorate, mode, ignorePosition, standalone);
    }

    void* findFn(HANDLE handle, const std::string& name, const std::string& needle) {
        for (const auto& fn : HyprlandAPI::findFunctionsByName(handle, name)) {
            if (fn.demangled.find(needle) != std::string::npos)
                return fn.address;
        }
        throw std::runtime_error("[hyprgrid] span: no function " + needle);
    }
}

void Span::init(HANDLE handle) {
    g_shouldRenderHook = HyprlandAPI::createFunctionHook(
        handle, findFn(handle, "shouldRenderWindow", "IHyprRenderer::shouldRenderWindow(Hyprutils::Memory::CSharedPointer<Desktop::View::CWindow>, Hyprutils::Memory::CSharedPointer<Monitor::CMonitor>)"),
        rc<void*>(hkShouldRenderWindow));
    g_renderWindowHook = HyprlandAPI::createFunctionHook(handle, findFn(handle, "renderWindow", "IHyprRenderer::renderWindow(Render::CRenderContext&"), rc<void*>(hkRenderWindow));
    if (!g_shouldRenderHook || !g_renderWindowHook || !g_shouldRenderHook->hook() || !g_renderWindowHook->hook()) {
        exit();
        throw std::runtime_error("[hyprgrid] span: failed hooking the renderer");
    }
}

void Span::exit() {
    for (auto* hook : {g_renderWindowHook, g_shouldRenderHook}) {
        if (hook)
            hook->unhook();
    }
    g_fades.clear();
}

void Span::viewChanged(const PHLMONITOR& monitor) {
    if (!monitor || !g_pHyprRenderer)
        return;
    std::erase_if(g_fades, [](const auto& entry) { return !entry.second.window.lock(); });
    for (const auto& window : Desktop::windowState()->windows()) {
        if (!window || !window->isFloating() || !window->m_workspace || window->m_workspace->m_monitor.lock() != monitor)
            continue;
        const auto BOX = window->getFullWindowBoundingBox();
        const auto ON  = BOX.intersection(monitor->logicalBox());
        if (ON.w < BOX.w || ON.h < BOX.h)
            damage(window); // lies partly elsewhere
    }
}
