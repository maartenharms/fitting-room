#include "IconImages.h"

#include "BuildChannel.h"

#include <array>
#include <string>

namespace OS::IconImages {

    namespace {

        // ⚠ INDEXED BY WeaponClass. The order is WeaponSlots.h's enum order and
        // the static_assert below is what keeps it that way; a class added in
        // the middle would otherwise silently give every later row the wrong
        // picture, which is worse than no picture at all.
        constexpr const char* kWeaponStem[] = {
            "sword",     // Sword
            "dagger",    // Dagger
            "waraxe",    // WarAxe
            "mace",      // Mace
            "greatsword",// Greatsword
            "battleaxe", // BattleaxeWarhammer
            "bow",       // Bow
            "crossbow",  // Crossbow
            "staff",     // Staff
            "arrows",    // Arrows
            "bolts",     // Bolts
        };
        static_assert(std::size(kWeaponStem) == kWeaponClassCount,
                      "kWeaponStem must have one entry per WeaponClass");

        // The path shape FUCK's own WidgetTemplate uses: Data-relative, which
        // resolves wherever the mod manager has staged us. dist/ puts these
        // beside icons.ttf.
        FUCK::Image Load(const char* a_stem) {
            const auto path = (BuildChannel::DataPath("icons") /
                               (std::string{ a_stem } + ".png")).string();
            FUCK::Image img(path.c_str());
            if (img.IsLoaded()) {
                spdlog::debug("IconImages: loaded '{}' ({:.0f}x{:.0f}).", path,
                              img.GetWidth(), img.GetHeight());
            } else {
                spdlog::warn("IconImages: '{}' did not load - that row keeps its glyph.",
                             path);
            }
            return img;
        }

        // Biped slot BIT -> picture stem, for the rows no free-solid codepoint
        // can serve. Bits 19 and 22 are pelvis outer and under, 23 and 24 are
        // the two legs, 27 is the shoulder, 28 and 29 are the two arms. Both
        // members of each pair point at the same stem on purpose - see the
        // header - and the arms differ only by the mirror flag below.
        struct SlotPic {
            std::uint32_t bit;
            const char*   stem;
            // Drawn right-to-left. ⚠ THE MIRROR IS WHY THE ARMS ARE PICTURES AT
            // ALL. Both arm rows were the same hand-rock glyph, so a left arm
            // and a right arm were the same mark twice over, and a glyph cannot
            // be flipped - text has no UV to swap. One drawing sampled backwards
            // costs nothing and makes the pair read as a pair (user 2026-08-05).
            bool mirror;
        };
        constexpr SlotPic kSlotPic[] = {
            { 19, "pelvis", false }, { 22, "pelvis", false },
            { 23, "leg", false },    { 24, "leg", false },
            { 27, "shoulder", false },
            { 28, "arm", true },     // Arm (left), slot 58
            { 29, "arm", false },    // Arm (right), slot 59
        };

        struct Store {
            std::array<FUCK::Image, kWeaponClassCount> weapon;
            FUCK::Image                                hair;
            FUCK::Image                                frame;
            FUCK::Image                                frameFill;
            // One entry per kSlotPic ROW, not per distinct stem. Two rows
            // naming the same file load it twice, which costs one extra
            // texture and keeps the lookup a straight walk of the table
            // rather than a second stem-to-index map to keep in step.
            std::array<FUCK::Image, std::size(kSlotPic)> slot;
        };

        // ⚠ DELIBERATELY NEVER DESTROYED. FUCK::Image's destructor calls back
        // into FUCK to release the texture, and a function-local static would
        // run that at process exit, after the host may already be gone. The
        // textures live for the session either way, so leaking the store is the
        // cheap way to guarantee we never call into an unloaded DLL.
        Store& Get() {
            static Store* const store = [] {
                auto* s = new Store{};
                for (std::size_t i = 0; i < kWeaponClassCount; ++i) {
                    s->weapon[i] = Load(kWeaponStem[i]);
                }
                s->hair  = Load("hair");
                s->frame     = Load("frame");
                s->frameFill = Load("frame_fill");
                for (std::size_t i = 0; i < std::size(kSlotPic); ++i) {
                    s->slot[i] = Load(kSlotPic[i].stem);
                }
                return s;
            }();
            return *store;
        }

    }  // namespace

    ImTextureID ForWeapon(WeaponClass a_class) {
        const auto i = static_cast<std::size_t>(a_class);
        if (i >= kWeaponClassCount) {
            return static_cast<ImTextureID>(0);
        }
        auto& img = Get().weapon[i];
        return img.IsLoaded() ? img.GetID() : static_cast<ImTextureID>(0);
    }

    ImTextureID ForHair() {
        auto& img = Get().hair;
        return img.IsLoaded() ? img.GetID() : static_cast<ImTextureID>(0);
    }

    ImTextureID Frame() {
        auto& img = Get().frame;
        return img.IsLoaded() ? img.GetID() : static_cast<ImTextureID>(0);
    }

    ImTextureID FrameFill() {
        auto& img = Get().frameFill;
        return img.IsLoaded() ? img.GetID() : static_cast<ImTextureID>(0);
    }

    ImTextureID ForSlotBit(std::uint32_t a_bit) {
        for (std::size_t i = 0; i < std::size(kSlotPic); ++i) {
            if (kSlotPic[i].bit != a_bit) {
                continue;
            }
            auto& img = Get().slot[i];
            return img.IsLoaded() ? img.GetID() : static_cast<ImTextureID>(0);
        }
        return static_cast<ImTextureID>(0);
    }

    bool MirrorSlotBit(std::uint32_t a_bit) {
        for (const auto& e : kSlotPic) {
            if (e.bit == a_bit) {
                return e.mirror;
            }
        }
        return false;
    }

}  // namespace OS::IconImages
