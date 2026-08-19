#pragma once

namespace sunrise::core::ui::hud::overlays::objective {

/**
 * Draws a marker over the world at the roteiro's next beat.
 *
 * Unlike the stacked overlays this one owns no corner of the screen: it draws into the viewport's own
 * foreground, at wherever the beat happens to project to. The overlay stack calls it outside a window
 * for exactly that reason.
 */
void draw() noexcept;

} // namespace sunrise::core::ui::hud::overlays::objective
