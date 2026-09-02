#pragma once

// One body preset, turned into the scene that photographs it.
//
// ⚠⚠ THIS EXISTS BECAUSE A SECOND PANE ASKED FOR THE SAME PICTURE. The rules
// below were paid for one field round at a time and none of them is guessable
// from the outside: which of two morph sources answers, which loader the
// leading path needs, that the morph must not reach the head, hands and feet,
// and that the character's weight is snapped before anything is built. A
// second copy of that in another file would drift on the first change, and
// every one of these rules is invisible when it is wrong.
//
// The split is per FRAME versus per CARD. MakeContext walks the mannequin's
// resolved parts and reads the character's weight, so it runs once; Build runs
// per row, inside a card callback that a grid calls for everything on screen.

#include "BodyPreset.h"         // BodySliderValue
#include "BodySlideCatalog.h"   // BodySlideCatalogSnapshot
#include "PreviewGrid.h"        // SceneIdentity

#include <string>
#include <string_view>
#include <vector>

namespace RE {
    class Actor;
}

namespace OS::BodyCardScene {

    struct Context {
        const BodySlideCatalogSnapshot* catalog{ nullptr };
        // The head, hands and feet a BodySlide reference mesh does not carry.
        std::vector<std::string> extras;
        // The torso the character is actually wearing, which is the fallback
        // subject when a preset's slider set is not installed.
        std::string bodyPath;
        // Snapped, so a weight drag does not rebuild every card at every value
        // the handle passes through. See BodyMorphData::SnapWeight.
        float weight01{ 1.0f };
        // ⚠ THE CATALOG SCAN IS ASYNC. Until it lands, a set with a perfectly
        // good .osp looks like it has none, and falling back to the built body
        // then would photograph the wrong subject and cache it for good.
        bool ready{ false };
    };

    [[nodiscard]] Context MakeContext(const BodySlideCatalogSnapshot* a_catalog,
                                      RE::Actor*                      a_target);

    struct Scene {
        PreviewGrid::SceneIdentity id;
        bool                       ok{ false };
        // A translation key naming why there is no picture, for the card's own
        // tooltip. A cross that explains itself only in a log is not an
        // explanation; this pane cost four field rounds proving that.
        const char* noCardKey{ nullptr };
    };

    [[nodiscard]] Scene Build(const Context& a_ctx, std::string_view a_presetName,
                              const std::vector<BodySliderValue>& a_sliders,
                              const std::string&                  a_setName);

}  // namespace OS::BodyCardScene
