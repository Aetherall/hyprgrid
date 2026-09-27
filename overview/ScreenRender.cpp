// Drawing a screen of the overview (see Screen.hpp): the wallpaper, each
// workspace on its cell with its windows live, the grid, the seam, where the
// other monitors look, and a dragged window at the pointer.

#include "Internal.hpp"
#include "Session.hpp"
#include "OverviewPassElement.hpp"
#include "OverviewRender.hpp"
#include "TopologyConfig.hpp"
#include "View.hpp"
#include "Window.hpp"

namespace Overview {
    // --- Layers ---

    // A layer level drawn into a workspace's box, scaled like it.
    static void renderLayerLevel(const PHLMONITOR& monitor, uint32_t layer, const CBox& box, float scale, const Time::steady_tp& now, float alpha = 1.F) {
        if (!monitor)
            return;

        bool       pushedHints   = false;
        const bool MODULATEALPHA = alpha < 0.999F;
        for (const auto& ls : monitor->m_layerSurfaceLayers[layer]) {
            const auto LAYER = ls.lock();
            if (!Desktop::View::validMapped(LAYER))
                continue;

            if (!pushedHints) {
                Render::SRenderModifData modif;
                modif.modifs.emplace_back(Render::SRenderModifData::RMOD_TYPE_SCALE, scale);
                modif.modifs.emplace_back(Render::SRenderModifData::RMOD_TYPE_TRANSLATE, box.pos());
                g_pHyprRenderer->m_renderPass.add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = modif}));
                pushedHints = true;
            }

            auto& fade     = LAYER->alpha()[Desktop::View::LS_ALPHA_FADE];
            float previous = 1.F;
            if (MODULATEALPHA && fade->value()) {
                previous = fade->value();
                fade->setValueAndWarp(previous * std::clamp(alpha, 0.F, 1.F));
            }
            g_pHyprRenderer->renderLayer(LAYER, monitor, now);
            if (MODULATEALPHA && fade->value())
                fade->setValueAndWarp(previous);
        }

        if (pushedHints)
            g_pHyprRenderer->m_renderPass.add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = Render::SRenderModifData{}}));
    }

    void CScreen::renderWallpaperLayers(const CBox& box, float scale_, const Time::steady_tp& now, float alpha) {
        renderLayerLevel(monitor.lock(), ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, box, scale_, now, alpha);
    }

    void CScreen::renderGlobalWallpaper(const Time::steady_tp& now) {
        const auto MONITOR = monitor.lock();
        if (!MONITOR)
            return;
        g_pHyprRenderer->renderBackground(MONITOR);
        for (const auto& ls : MONITOR->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND]) {
            if (Desktop::View::validMapped(ls.lock()))
                g_pHyprRenderer->renderLayer(ls.lock(), MONITOR, now);
        }
    }

    // --- Blur ---

    // The wallpaper blurred once into a framebuffer, redone when it changes.
    void CScreen::updateBackdropBlurCache(int wallpaperMode, const Time::steady_tp& now) {
        const auto MONITOR = monitor.lock();
        if (!MONITOR || wallpaperMode == 1 || !Overview::Config::getBlur())
            return;

        if (lastBackdropWallpaperMode != wallpaperMode) {
            backdropBlurDirty         = true;
            lastBackdropWallpaperMode = wallpaperMode;
        }

        const auto SIZE   = MONITOR->m_transformedSize;
        const auto FORMAT = g_pHyprRenderer->m_renderData.currentFB->m_drmFormat;
        if (!backdropBlurFB)
            backdropBlurFB = g_pHyprRenderer->createFB("hyprgrid_overview_backdrop_blur");
        if (!backdropBlurFB || !backdropBlurFB->isAllocated() || backdropBlurFB->m_size != SIZE || backdropBlurFB->m_drmFormat != FORMAT) {
            if (backdropBlurFB)
                backdropBlurFB->release();
            if (!backdropBlurFB || !backdropBlurFB->alloc(sc<int>(SIZE.x), sc<int>(SIZE.y), FORMAT))
                return;
            backdropBlurDirty = true;
        }
        if (!backdropBlurDirty)
            return;

        if (g_pHyprRenderer->m_renderData.currentFB)
            backdropBlurFB->setImageDescription(g_pHyprRenderer->m_renderData.currentFB->imageDescription());

        const CRegion FULL{CBox{0, 0, SIZE.x, SIZE.y}};
        {
            auto bind = g_pHyprRenderer->bindTempFB(backdropBlurFB);
            g_pHyprRenderer->draw(CClearPassElement::SClearData{CHyprColor{0.F, 0.F, 0.F, 1.F}}, FULL);
            renderGlobalWallpaper(now);
            OverviewRender::flushPass(MONITOR);
        }

        auto                 blurDamage = FULL;
        SP<Render::ITexture> blurred;
        {
            auto       bind = g_pHyprRenderer->bindTempFB(backdropBlurFB);
            const auto FB   = g_pHyprRenderer->blurMainFramebuffer(1.F, blurDamage);
            blurred         = FB ? FB->getTexture() : nullptr;
        }
        if (!blurred || !blurred->m_size.x || !blurred->m_size.y)
            return;

        {
            auto bind = g_pHyprRenderer->bindTempFB(backdropBlurFB);
            g_pHyprRenderer->draw(CClearPassElement::SClearData{CHyprColor{0.F, 0.F, 0.F, 0.F}}, FULL);
            g_pHyprRenderer->draw(CTexPassElement::SRenderData{.tex = blurred, .box = CBox{0, 0, SIZE.x, SIZE.y}, .damage = FULL}, FULL);
        }
        backdropBlurDirty = false;
    }

    void CScreen::renderBackdropBlurCache() {
        const auto MONITOR = monitor.lock();
        if (!MONITOR || !backdropBlurFB || !backdropBlurFB->isAllocated() || !backdropBlurFB->getTexture())
            return;
        const auto    SIZE = MONITOR->m_transformedSize;
        const CRegion FULL{CBox{0, 0, SIZE.x, SIZE.y}};
        g_pHyprRenderer->draw(CTexPassElement::SRenderData{.tex = backdropBlurFB->getTexture(), .box = CBox{0, 0, SIZE.x, SIZE.y}, .a = 1.F, .damage = FULL}, FULL);
    }

    // Windows drawing Hyprland's precomputed blur behind them need it
    // computed first.
    bool CScreen::hasVisiblePrecomputedBlurWindow(float scale_) const {
        const auto MONITOR = monitor.lock();
        if (!MONITOR)
            return false;

        const auto DRAGGED = overview()->dragged();
        for (const auto& image : images) {
            const auto BOX = workspaceBox(image);
            if (!onMonitor(visibleBox(image, BOX, scale_), MONITOR))
                continue;

            const auto blurs = [&](const PHLWINDOW& window) {
                return window != DRAGGED && OverviewWindow::shouldUseBlurFramebuffer(window) && onMonitor(windowBox(window, image), MONITOR);
            };
            if (const auto FS = fullscreenOf(image.workspace)) {
                if (blurs(FS))
                    return true;
                continue;
            }
            for (const auto& ref : image.windows) {
                if (blurs(shown(ref.lock())))
                    return true;
            }
        }
        return false;
    }

    // --- Card shadows ---

    struct SShadowConfig {
        bool                         enabled     = false;
        int                          range       = 0;
        int                          renderPower = 1;
        ::Config::CGradientValueData color;
    };

    static SShadowConfig shadowConfig() {
        const auto RANGE       = Overview::Config::getShadowRange();
        const auto POWER       = Overview::Config::getShadowRenderPower();
        const auto COLOR       = Overview::Config::getShadowColor();
        const auto GLOBALCOLOR = Overview::Config::getValue<::Config::CGradientValueData>("decoration:shadow:color");

        ::Config::CGradientValueData color;
        if (COLOR && !COLOR->m_colors.empty())
            color = *COLOR;
        else if (!GLOBALCOLOR.m_colors.empty())
            color = GLOBALCOLOR;

        return {
            .enabled     = !!Overview::Config::getShadowEnabled(),
            .range       = std::max(0, RANGE >= 0 ? RANGE : Overview::Config::getValue<int>("decoration:shadow:range")),
            .renderPower = std::clamp(POWER >= 0 ? POWER : Overview::Config::getValue<int>("decoration:shadow:render_power"), 1, 4),
            .color       = color,
        };
    }

    static void renderWorkspaceShadow(const PHLMONITOR& monitor, const CBox& box, float scale, bool cutoutCentre, float alpha = 1.F) {
        const auto SHADOW  = shadowConfig();
        const bool VISIBLE = std::ranges::any_of(SHADOW.color.m_colors, [](const CHyprColor& color) { return color.a > 0.F; });
        if (!monitor || !SHADOW.enabled || SHADOW.range <= 0 || !VISIBLE || alpha <= 0.F)
            return;

        const int RANGE = sc<int>(std::round(SHADOW.range * monitor->m_scale * scale));
        auto      base  = box.copy().round();
        if (RANGE <= 0 || base.width < 1 || base.height < 1)
            return;

        g_pHyprRenderer->m_renderPass.add(makeUnique<COverviewShadowPassElement>(COverviewShadowPassElement::SData{
            .monitor       = monitor,
            .fullBox       = base.copy().expand(RANGE).round(),
            .cutoutBox     = base,
            .rounding      = 0,
            .roundingPower = 2.F,
            .range         = RANGE,
            .renderPower   = SHADOW.renderPower,
            .color         = SHADOW.color,
            .alpha         = alpha,
            .ignoreWindow  = cutoutCentre,
        }));
    }

    // --- Workspaces ---

    // A cell's box grown by how far its windows reach past it: what culling
    // must keep.
    CBox CScreen::visibleBox(const SWorkspaceImage& image, const CBox& box, float scale_) const {
        const auto MONITOR = monitor.lock();
        if (!MONITOR)
            return box;
        return toCBox(Scene::grow(sceneCamera(MONITOR, scale_, {}), sceneBox(box), image.overflow), false);
    }

    // Workspaces draw as visible while zoomed out; restored after.
    static Hyprutils::Utils::CScopeGuard forceRendering(const PHLWORKSPACE& workspace) {
        const bool VISIBLE = workspace->visible(), FORCED = workspace->m_forceRendering;
        workspace->setVisible(true);
        workspace->m_forceRendering = true;
        return Hyprutils::Utils::CScopeGuard([workspace, VISIBLE, FORCED] {
            workspace->setVisible(VISIBLE);
            workspace->m_forceRendering = FORCED;
        });
    }

    void CScreen::renderWorkspaceBackground(const SWorkspaceImage& image, float scale_, int wallpaperMode, const Time::steady_tp& now) {
        const auto MONITOR = monitor.lock();
        const auto BOX     = workspaceBox(image);
        const auto ALPHA   = alphaOf(image);
        if (!onMonitor(BOX, MONITOR))
            return;

        auto restore = forceRendering(image.workspace);

        renderWorkspaceShadow(MONITOR, BOX, scale_, wallpaperMode == 0, ALPHA);
        if (Overview::Config::getBlur() && wallpaperMode != 1 && ALPHA > 0.001F)
            OverviewRender::queueBlur(BOX, 0, 2.F, ALPHA, false);
        if (wallpaperMode != 0 && ALPHA > 0.001F)
            renderWallpaperLayers(BOX, scale_, now, ALPHA);
        if (ALPHA >= 0.999F)
            renderLayerLevel(MONITOR, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, BOX, scale_, now);
    }

    void CScreen::renderWorkspaceLive(const SWorkspaceImage& image, float scale_, const Time::steady_tp& now) {
        const auto MONITOR = monitor.lock();
        const auto BOX     = workspaceBox(image);
        if (!onMonitor(visibleBox(image, BOX, scale_), MONITOR))
            return;

        auto       restore = forceRendering(image.workspace);
        const auto DRAGGED = overview()->dragged();

        const auto renderWindow = [&](const PHLWINDOW& window) {
            if (!isShown(window) || window == DRAGGED)
                return;
            const auto WINDOWBOX = windowBox(window, image);
            if (onMonitor(WINDOWBOX, MONITOR))
                renderWindowLive(window, WINDOWBOX, scale_, now, &BOX);
        };

        // Where the dragged window would land, on the workspace under it.
        const auto renderDropIndicator = [&] {
            if (hasRunningWorkspaceAnimation() || !DRAGGED)
                return;
            if (overview()->screenAt(g_pInputManager->getMouseCoordsInternal()).get() != this)
                return;
            if (dropWorkspaceAt(pointerLocal, DRAGGED) != image.workspace)
                return;

            CDropIndicator::renderDropIndicator({
                .monitor               = MONITOR,
                .workspace             = image.workspace,
                .workspaceUsableBox    = usableBox(image),
                .anchor                = dropAnchorAt(image, DRAGGED),
                .renderScale           = scale_,
                .workspaceFullyVisible = fullyOnMonitor(BOX, MONITOR),
                .floating              = DRAGGED->isFloating(),
            });
        };

        // Fullscreen: only it and the floating windows over it.
        if (const auto FS = fullscreenOf(image.workspace)) {
            renderWindow(FS);
            OverviewRender::flushPass(MONITOR);
            for (const auto& ref : image.windows) {
                const auto WINDOW = shown(ref.lock());
                if (isShown(WINDOW) && WINDOW->isFloating() && WINDOW != FS)
                    renderWindow(WINDOW);
            }
            renderDropIndicator();
            return;
        }

        for (const bool FLOATING : {false, true}) {
            for (const auto& ref : image.windows) {
                const auto WINDOW = shown(ref.lock());
                if (WINDOW && WINDOW->isFloating() == FLOATING)
                    renderWindow(WINDOW);
            }
        }
        renderDropIndicator();
    }

    // --- Windows ---

    void CScreen::renderWindowLive(const PHLWINDOW& window, const CBox& box, float scale_, const Time::steady_tp& now, const CBox* workspaceBox_, bool dragged) {
        if (!window)
            return;

        // The dragged window, or one just dropped, drawn as focused.
        const auto DRAGGED = overview()->dragged();

        forceWindowVisible(window);
        forceWindowSurfacesVisible(window);

        OverviewWindow::renderOverviewWindow({
            .monitor           = monitor.lock(),
            .window            = window,
            .windowBox         = box,
            .renderScale       = scale_,
            .now               = now,
            .workspaceBox      = workspaceBox_,
            .selected          = selection == window,
            .dragged           = dragged,
            .pseudoFocusWindow = DRAGGED ? DRAGGED : overview()->pseudoFocused(now),
        });
    }

    // The dragged window under the pointer, on whichever screen it is over.
    void CScreen::renderDraggedWindow(const Time::steady_tp& now) {
        const auto MONITOR = monitor.lock();
        const auto WINDOW  = overview()->dragged();
        if (!MONITOR || !isShown(WINDOW) || !WINDOW->m_workspace)
            return;

        const auto GLOBAL = overview()->draggedGlobalBox();
        const auto AREA   = MONITOR->logicalBox();
        if (GLOBAL.empty() || AREA.empty() || GLOBAL.intersection(AREA).empty())
            return;

        const auto SOURCE = overview()->drag.source.lock();
        renderWindowLive(WINDOW, CBox{(GLOBAL.pos() - MONITOR->m_position) * MONITOR->m_scale, GLOBAL.size() * MONITOR->m_scale}, SOURCE ? SOURCE->zoomScale() : zoomScale(), now,
                         nullptr, true);
    }

    // Pinned floating windows keep to a corner of the screen, shrunk only as
    // far as they must to stay clear of the grid.
    static CBox pinnedFloatingBox(const PHLMONITOR& monitor, const PHLWINDOW& window, float targetScale, float progress_, float* renderScale) {
        *renderScale     = 1.F;
        const auto MS    = monitor->m_scale;
        const auto SIZE  = window->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT) * MS;
        if (SIZE.x <= 0 || SIZE.y <= 0)
            return {};

        const CBox BOX = {(window->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT) - monitor->m_position) * MS, SIZE};
        const auto W = sc<float>(monitor->m_size.x * MS), H = sc<float>(monitor->m_size.y * MS);

        const auto QUADRANT = Interaction::quadrant(sceneBox(BOX), {W, H});
        const bool RIGHT = !QUADRANT.left, BOTTOM = !QUADRANT.top;

        const auto  FULL = monitor->logicalBox(), WORK = monitor->logicalBoxMinusReserved();
        const float RLEFT   = std::max(0.F, sc<float>(WORK.x - FULL.x)) * MS;
        const float RTOP    = std::max(0.F, sc<float>(WORK.y - FULL.y)) * MS;
        const float RRIGHT  = std::max(0.F, sc<float>((FULL.x + FULL.width) - (WORK.x + WORK.width))) * MS;
        const float RBOTTOM = std::max(0.F, sc<float>((FULL.y + FULL.height) - (WORK.y + WORK.height))) * MS;

        const auto GAP       = sc<float>(Overview::Config::getWorkspaceGap()) * MS;
        const auto ROOM      = std::max(1.F, sc<float>((W - W * targetScale) / 2.F - 2.F * GAP - (RIGHT ? RRIGHT : RLEFT)));
        const auto TARGET    = std::min(1.F, std::max(ROOM / sc<float>(SIZE.x), targetScale));
        const auto PROGRESS  = std::clamp(progress_, 0.F, 1.F);
        const auto CURRENT   = 1.F + (TARGET - 1.F) * PROGRESS;
        const auto TW = sc<float>(SIZE.x) * CURRENT, TH = sc<float>(SIZE.y) * CURRENT;
        *renderScale = CURRENT;

        const float X = RIGHT ? W - TW - GAP - RRIGHT : GAP + RLEFT;
        const float Y = BOTTOM ? H - TH - GAP - RBOTTOM : GAP + RTOP;
        CBox        box{{BOX.x + (X - BOX.x) * PROGRESS, BOX.y + (Y - BOX.y) * PROGRESS}, {TW, TH}};
        box.round();
        return box;
    }

    void CScreen::renderPinnedFloatingWindows(float scale_, const Time::steady_tp& now) {
        const auto MONITOR = monitor.lock();
        if (!MONITOR)
            return;

        // How far into the zoom, from the view's scale.
        const auto TARGET   = Overview::Config::getScale();
        const auto PROGRESS = (1.F - TARGET) > 0.001F ? (1.F - scale_) / (1.F - TARGET) : 1.F;
        const auto DRAGGED  = overview()->dragged();
        for (const auto& ref : pinnedFloatingWindows) {
            const auto WINDOW = shown(ref.lock());
            if (!isPinnedFloating(WINDOW) || WINDOW == DRAGGED || WINDOW->m_monitor != MONITOR)
                continue;
            float      renderScale = 1.F;
            const auto BOX         = pinnedFloatingBox(MONITOR, WINDOW, TARGET, PROGRESS, &renderScale);
            if (onMonitor(BOX, MONITOR))
                renderWindowLive(WINDOW, BOX, renderScale, now);
        }
    }

    // --- The grid ---

    static CHyprColor activeBorderColour() {
        auto* const BORDER = sc<::Config::CGradientValueData*>(Overview::Config::valueRef<::Config::IComplexConfigValue>("general:col.active_border").ptr());
        return BORDER && !BORDER->m_colors.empty() ? BORDER->m_colors[0] : Colors::WHITE;
    }

    static void renderOutline(const CBox& box, double thickness, const CHyprColor& colour) {
        for (const auto& edge : {CBox{box.x, box.y, box.width, thickness}, CBox{box.x, box.y + box.height - thickness, box.width, thickness}, CBox{box.x, box.y, thickness, box.height},
                                 CBox{box.x + box.width - thickness, box.y, thickness, box.height}}) {
            CRectPassElement::SRectData data;
            data.box   = edge;
            data.color = colour;
            g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(data));
        }
    }

    // While a window is dragged: the empty cells it can open a workspace in,
    // the one under the pointer brighter.
    void CScreen::renderDropSlots(float scale_) {
        const auto MONITOR = monitor.lock();
        if (!MONITOR || !overview()->dragging())
            return;

        const auto COLOUR   = activeBorderColour();
        const auto ROUNDING = sc<int>(std::round(Overview::Config::getValue<int>("decoration:rounding") * scale_ * MONITOR->m_scale));
        const auto HOVERED  = slotAt(pointerLocal);
        for (const auto& slot : dropSlots()) {
            const auto BOX = slotBox(slot);
            if (BOX.empty() || !onMonitor(BOX, MONITOR))
                continue;
            CRectPassElement::SRectData data;
            data.box           = BOX;
            data.color         = COLOUR.modifyA(HOVERED && *HOVERED == slot ? 0.35F : 0.12F);
            data.round         = std::max(0, ROUNDING);
            data.roundingPower = Overview::Config::getValue<float>("decoration:rounding_power");
            g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(data));
        }
    }

    // Monitors on a shared board: a frame over where each one looks, this
    // one's in the border colour, the others dimmer.
    void CScreen::renderMonitorFrames(float scale_) {
        const auto MONITOR = monitor.lock();
        const auto ORIGIN  = Grid::cellOf(startedOn);
        std::vector<PHLMONITOR> onBoard;
        for (const auto& mon : State::monitorState()->monitors()) {
            if (Grid::sameBoard(mon, MONITOR))
                onBoard.push_back(mon);
        }
        if (!MONITOR || onBoard.size() < 2 || !ORIGIN)
            return;

        const auto COLOUR    = activeBorderColour();
        const auto THICKNESS = std::max(2.0, std::round(3.0 * MONITOR->m_scale));
        auto       camera    = sceneCamera(MONITOR, scale_, viewOffset->value());
        camera.origin        = *ORIGIN;
        for (const auto& mon : onBoard) {
            // Where that monitor's view is, mid-glide too, in this screen's cells.
            const auto BOX = toCBox(Scene::frameBox(camera, sceneMonitor(mon), View::position(mon), mon == MONITOR));
            if (!BOX.empty() && onMonitor(BOX, MONITOR))
                renderOutline(BOX, THICKNESS, COLOUR.modifyA(mon == MONITOR ? 0.95F : 0.4F));
        }
    }

    // Every empty cell in and around the workspaces' extent, faintly: the grid
    // the workspaces sit on.
    void CScreen::renderGridCells() {
        const auto MONITOR = monitor.lock();
        if (!MONITOR)
            return;
        std::vector<Grid::SCell> cells;
        for (const auto& image : images)
            cells.push_back(image.cell);
        const auto THICKNESS = std::max(1.0, double(std::round(MONITOR->m_scale)));
        for (const auto& cell : Scene::outlineCells(cells)) {
            const auto BOX = slotBox(cell);
            if (!BOX.empty() && onMonitor(BOX, MONITOR))
                renderOutline(BOX, THICKNESS, CHyprColor{1.0, 1.0, 1.0, 0.07});
        }
    }

    // The seam between the two monitors' regions, across the whole screen.
    void CScreen::renderSeam(float scale_) {
        const auto MONITOR = monitor.lock();
        const auto SEAM    = TopologyConfig::seam();
        const auto ORIGIN  = Grid::cellOf(startedOn);
        if (!MONITOR || !SEAM || !ORIGIN)
            return;
        auto camera   = sceneCamera(MONITOR, scale_, viewOffset->value());
        camera.origin = *ORIGIN;
        CRectPassElement::SRectData data;
        data.box   = toCBox(Scene::seamLine(camera, *SEAM, std::max(2.0, std::round(3.0 * MONITOR->m_scale))), false);
        data.color = CHyprColor{1.0, 1.0, 1.0, 0.55};
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(data));
    }

    // --- A frame ---

    void CScreen::render() {
        const auto MONITOR = monitor.lock();
        if (!MONITOR)
            return;

        // Another screen holds the pointer (a drag crossing over): follow it here.
        if (const auto GRAB = overview()->grab(); GRAB && GRAB.get() != this && overview()->dragging())
            updatePointer();

        const bool PREVBLOCK                     = g_pHyprRenderer->m_bBlockSurfaceFeedback;
        g_pHyprRenderer->m_bBlockSurfaceFeedback = true;
        auto restoreFeedback = Hyprutils::Utils::CScopeGuard([PREVBLOCK] { g_pHyprRenderer->m_bBlockSurfaceFeedback = PREVBLOCK; });

        const auto NOW    = Time::steadyNow();
        const auto SCALE  = scale->value();
        const auto OFFSET = viewOffset->value();
        if (!overviewBlurStateValid || std::abs(lastOverviewBlurScale - SCALE) > 0.001F || lastOverviewBlurViewOffset.distanceSq(OFFSET) > 0.001F) {
            overviewBlurDirty          = true;
            overviewBlurStateValid     = true;
            lastOverviewBlurScale      = SCALE;
            lastOverviewBlurViewOffset = OFFSET;
        }

        // The wallpaper: blurred (cached), plain, or black (mode 1: per card only).
        const auto WALLPAPER = Overview::Config::getWallpaperMode();
        if (Overview::Config::getBlur() && WALLPAPER != 1) {
            updateBackdropBlurCache(WALLPAPER, NOW);
            if (backdropBlurFB && backdropBlurFB->isAllocated() && backdropBlurFB->getTexture())
                renderBackdropBlurCache();
            else
                renderGlobalWallpaper(NOW);
        } else if (WALLPAPER == 0 || WALLPAPER == 2)
            renderGlobalWallpaper(NOW);
        else
            g_pHyprRenderer->draw(CClearPassElement::SClearData{CHyprColor{0.F, 0.F, 0.F, 1.F}}, {});

        Event::bus()->m_events.render.stage.emit(RENDER_POST_WALLPAPER);

        for (const auto& image : images)
            renderWorkspaceBackground(image, SCALE, WALLPAPER, NOW);

        renderGridCells();
        renderDropSlots(SCALE);

        // A removed workspace fading out in its old cell.
        if (insertTransition.active && !insertTransition.fadeIn) {
            const auto ALPHA  = 1.F - std::clamp(insertFadeProgress->value(), 0.F, 1.F);
            auto       camera = sceneCamera(MONITOR, SCALE, OFFSET);
            camera.origin     = Grid::cellOf(startedOn).value_or(Grid::SCell{});
            const auto GHOST  = toCBox(Scene::cellBox(camera, insertTransition.removedCell));
            if (ALPHA > 0.001F && onMonitor(GHOST, MONITOR)) {
                renderWorkspaceShadow(MONITOR, GHOST, SCALE, WALLPAPER == 0, ALPHA);
                if (WALLPAPER != 0)
                    renderWallpaperLayers(GHOST, SCALE, NOW, ALPHA);
                renderLayerLevel(MONITOR, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, GHOST, SCALE, NOW);
            }
        }

        const bool PRECOMPUTEDBLUR = hasVisiblePrecomputedBlurWindow(SCALE);
        if (PRECOMPUTEDBLUR && overviewBlurDirty)
            g_pHyprRenderer->m_renderPass.add(makeUnique<CPreBlurElement>());
        OverviewRender::flushPass(MONITOR);
        if (PRECOMPUTEDBLUR)
            overviewBlurDirty = false;

        for (const auto& image : images)
            renderWorkspaceLive(image, SCALE, NOW);

        renderSeam(SCALE);
        renderMonitorFrames(SCALE);
        renderDraggedWindow(NOW);
        renderPinnedFloatingWindows(SCALE, NOW);

        for (const auto LAYER : {ZWLR_LAYER_SHELL_V1_LAYER_TOP, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY}) {
            for (const auto& ls : MONITOR->m_layerSurfaceLayers[LAYER]) {
                if (Desktop::View::validMapped(ls.lock()))
                    g_pHyprRenderer->renderLayer(ls.lock(), MONITOR, NOW);
            }
        }

        sendFrameCallbacks(NOW);
    }
}
