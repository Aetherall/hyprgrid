#include "DropIndicator.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

#define private public
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowPresentation.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#undef private

namespace {

static constexpr float INDICATOR_ALPHA   = 0.3F;

static CHyprColor indicatorColor() {
    auto* const ACTIVEBORDER = sc<Config::CGradientValueData*>(Overview::Config::valueRef<Config::IComplexConfigValue>("general:col.active_border").ptr());
    if (!ACTIVEBORDER || ACTIVEBORDER->m_colors.empty())
        return Colors::WHITE.modifyA(INDICATOR_ALPHA);

    return ACTIVEBORDER->m_colors[0].modifyA(INDICATOR_ALPHA);
}

static bool isHorizontalSide(const std::string& side) {
    return side == "l" || side == "r";
}

static bool isVerticalSide(const std::string& side) {
    return side == "u" || side == "d";
}

static CBox indicatorBox(const CDropIndicator::SRenderParams& params) {
    if (!params.anchor.window)
        return params.workspaceUsableBox;

    return params.anchor.box;
}

static int indicatorRounding(const CDropIndicator::SRenderParams& params) {
    const float ROUNDING = params.anchor.window ? params.anchor.window->presentation().rounding() : Overview::Config::getValue<int>("decoration:rounding");
    const float SCALE    = std::max(params.renderScale, 0.01F) * std::max<float>(params.monitor ? params.monitor->m_scale : 1.F, 0.01F);

    return std::max(0, sc<int>(std::round(ROUNDING * SCALE)));
}

static float indicatorRoundingPower(const CDropIndicator::SRenderParams& params) {
    if (params.anchor.window)
        return params.anchor.window->presentation().roundingPower();

    return Overview::Config::getValue<float>("decoration:rounding_power");
}

}

void CDropIndicator::renderDropIndicator(const SRenderParams& params) {
    if (!params.monitor || !params.workspace)
        return;

    if (!params.anchor.window && params.floating && params.workspaceFullyVisible)
        return;

    if (params.anchor.window && !params.anchor.direction.empty() && !isHorizontalSide(params.anchor.direction) && !isVerticalSide(params.anchor.direction))
        return;

    const auto BOX = indicatorBox(params);
    if (BOX.empty())
        return;

    CRectPassElement::SRectData data;
    data.box           = BOX;
    data.color         = indicatorColor();
    data.round         = indicatorRounding(params);
    data.roundingPower = indicatorRoundingPower(params);

    g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(data));
}
