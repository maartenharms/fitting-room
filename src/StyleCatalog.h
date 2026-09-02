#pragma once

#include "Outfit.h"
#include "PreviewGrid.h"  // TextureSwapEntry (pure)

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace OS {

    // Why a style may not render on the TARGET - the player by default, or
    // an NPC being edited once the editor's "Editing:" selector lands (see
    // EvaluateFitFor/RefreshFitFor). Drives the red-flag reason.
    enum class FitReason : std::uint8_t {
        kFits = 0,   // a race-valid armature with a mesh for the target's sex exists
        kNoRace,     // no armature is valid for the target's race (UBE etc.)
        kNoSex,      // a race-valid armature exists but none has a mesh for the
                     // target's sex (gender-exclusive armor on the wrong gender)
        kCrashed,    // previewing this style crashed the game on a prior launch
                     // (CrashGuard); flagged so the same crash can't recur.
                     // GLOBAL, not per-target: a crashing mesh crashes for
                     // whoever wears it.
    };

    // One indexed look - an ARMOR style or, since the weapon dimension landed,
    // a WEAPON/AMMO style. weaponClass is the discriminator: nullopt = armor
    // (slotMask/primaryBit/armorType meaningful), set = weapon or ammo (the
    // armor fields are unused). `form` is the one pointer valid for BOTH, so
    // shared paths never have to know which kind they hold.
    struct StyleItem {
        RE::TESBoundObject* form{ nullptr };  // always set: the ARMO, WEAP or AMMO
        StyleRefKey         key;
        std::string         name;
        std::string         source;      // plugin filename
        std::uint32_t       slotMask{ 0 };
        std::uint32_t       primaryBit{ 0 };  // the ONE slot row it lists under (SlotMask.h)
        std::uint8_t        armorType{ 0 };   // 0 light, 1 heavy, 2 clothing
        bool                fitsBody{ true };  // false = won't render (see fitReason)
        FitReason           fitReason{ FitReason::kFits };
        bool                isRecent{ false };  // source plugin newly added this launch (OS-26)
        std::string         edid;  // best-effort; empty on runtimes without EDID retention

        // Every OTHER record that collapsed into this one look during Build -
        // the enchanted variants, the WACCF/mod re-issues, anything sharing
        // this look's addon set (armor) or class+model+texture-swap (weapons).
        // `form` is only the lowest-FormID REPRESENTATIVE of that group.
        //
        // Load-bearing for the Collection filter: ownership is recorded per
        // RECORD, so a player who owns only "Magecore Robes of Destruction"
        // has never owned the plain record this row shows. Testing the
        // representative alone therefore hid the whole look (the reporter's
        // "it will not appear unless I have a copy of the unenchanted
        // version"). Ask LookCollected/IsLookCollected, never Knows(form).
        std::vector<RE::FormID> variantIds;

        // Weapon dimension (see WeaponSlots.h). Set = this is a WEAP/AMMO
        // style listing under that class instead of an armor slot bit.
        std::optional<WeaponClass> weaponClass;

        // The resolved model paths, cached OFF the render thread so a preview
        // build never walks the form graph. Weapons fill exactly one at
        // Build() (TESModelTextureSwap::GetModel is the whole resolution).
        // Armour fills zero to N during the FIT WALK (RefreshFitFor), because
        // the answer depends on the target's race and sex and Build() runs
        // before either is known; an unfit style keeps an empty list and its
        // card draws the cross.
        std::vector<std::string> modelPaths;

        // The paths' texture swaps (OS-192), parallel to modelPaths when
        // present and EMPTY when no path carries any, which is what keeps a
        // swap-less disk key byte-identical (the pinned rule). Filled where
        // modelPaths is filled, from the same model subobjects.
        std::vector<std::vector<PreviewGrid::TextureSwapEntry>> swaps;

        [[nodiscard]] bool HasPreviewScene() const { return !modelPaths.empty(); }

        [[nodiscard]] bool IsWeapon() const { return weaponClass.has_value(); }

        // The ARMO for an armor item, nullptr for a weapon one. Derived from
        // form rather than stored alongside it, so the two cannot disagree.
        [[nodiscard]] RE::TESObjectARMO* Armo() const {
            return IsWeapon() ? nullptr : static_cast<RE::TESObjectARMO*>(form);
        }
    };

    // Is this AMMO a bolt? NOT TESAmmo::IsBolt(): that helper reads the `data`
    // member at its SE offset, but AMMO's runtime data relocates (0x110 SE ->
    // 0x100 AE, see TESAmmo.h), so on AE it reads one byte past the end of the
    // object. GetRuntimeData() is the relocation-correct accessor. kNonBolt SET
    // means arrow - bolt-ness is the flag's absence.
    [[nodiscard]] bool IsBoltAmmo(RE::TESAmmo* a_ammo);

    // The styleable class of a live WEAP/AMMO, or nullopt for a form that has
    // none (a torch, a shield, hand-to-hand). The ONE place that maps the
    // engine's two weapon-ish form types onto WeaponClass: the catalog scan
    // that INDEXES a style under a class and the render hook that DECIDES a
    // class must agree exactly, or a style would render against a class it was
    // never listed under.
    [[nodiscard]] std::optional<WeaponClass> ClassOfWeaponForm(RE::TESForm* a_form);

    class StyleCatalog {
    public:
        static StyleCatalog& GetSingleton();

        // Scan every loaded ARMO. Call at kDataLoaded.
        void Build();

        // Re-evaluate fitsBody against the PLAYER's CURRENT race+sex. The
        // thin player-facing entry point over RefreshFitFor() - always marks
        // the fit cache's subject as the player, even if RefreshFitFor() last
        // cached against an NPC target (the editor calls this to RESTORE
        // player fit on close / when the "Editing:" target switches back to
        // the player). The race is only meaningful once the save is in (body
        // mods like UBE are custom races; at kDataLoaded the player still has
        // the main-menu default, which is why a Build()-time race filter
        // provably misses them - see research/ube-fit-detection.md). Call at
        // kPostLoadGame/kNewGame.
        void RefreshFit();

        // Re-evaluate fitsBody/fitReason against an ARBITRARY target's
        // race+sex (e.g. a follower selected in the editor's "Editing:"
        // selector). Same armature walk as RefreshFit(), just parameterized;
        // weapons are always kFits (no armature to walk). Records
        // (a_race, a_sexIdx) as the cache's current subject - FitRaceName(),
        // FitReasonText() and EnsureFitCurrent() all read that record, so a
        // tooltip drawn right after this call reads "(no <a_race> armature)"
        // / "no male mesh" for a_race/a_sexIdx, not the player's. Passing the
        // player's own live race+sex marks the subject as the player
        // (RefreshFit() funnels through this that way). MUST run on the main
        // thread and complete BEFORE the render thread draws catalog rows
        // again - same fitsBody-unsynchronized contract as EnsureFitCurrent.
        void RefreshFitFor(RE::TESRace* a_race, int a_sexIdx);

        // RefreshFit() only if the PLAYER's race OR SEX changed since the last
        // evaluation (RaceMenu swaps mid-save) AND the fit cache's
        // current subject IS the player. That second condition matters once
        // the editor can target an NPC via RefreshFitFor(): without it, this
        // would clobber a deliberately-cached follower fit back to the
        // player merely because it ran (e.g. at editor open) while an NPC
        // target's cache was live. When no NPC target is ever selected the
        // subject is always the player, so this behaves exactly as before.
        // Cheap when unchanged. Must run on the main thread while the editor
        // is CLOSED - Draw() reads fitsBody unsynchronized.
        void EnsureFitCurrent();

        // The shared render predicate: an armature that fits a_race AND
        // provides a mesh for a_sexIdx (RE::SEXES::kMale/kFemale). Used
        // per-catalog-item (via RefreshFitFor/RefreshFit) and for ARMOs
        // outside the catalog (showcase preset pieces, preset-fit tooltips).
        // Returns kFits when a_armo/a_race is unknowable (fail open, never a
        // false red). ARMOR ONLY - a weapon has no armature to walk, so
        // RefreshFit/RefreshFitFor never call this on one; they are kFits
        // unless CrashGuard flags them.
        [[nodiscard]] static FitReason EvaluateFitFor(RE::TESObjectARMO* a_armo,
                                                       RE::TESRace* a_race, int a_sexIdx);

        // Every addon of a_armo that passes the same race test EvaluateFitFor
        // applies (including the RNAM armorParentRace fallback) AND has a
        // mesh for a_sexIdx, in addon order. The multi-addon rule, decided in
        // the spec: render them all, one merged scene. Empty for an unfit
        // style, which is exactly "no scene". a_swaps fills in lockstep from
        // the SAME per-sex model subobject each path came from (OS-192), and
        // comes back EMPTY, not sized, when no addon carries any.
        static void CollectArmourPaths(
            RE::TESObjectARMO* a_armo, RE::TESRace* a_race, int a_sexIdx,
            std::vector<std::string>& a_out,
            std::vector<std::vector<PreviewGrid::TextureSwapEntry>>& a_swaps);

        // Player-facing convenience: EvaluateFitFor(a_armo, player's race,
        // player's sex). Unchanged signature/behavior from before the
        // per-target parameterization - a thin delegate.
        [[nodiscard]] static FitReason EvaluateFit(RE::TESObjectARMO* a_armo);
        [[nodiscard]] static bool      FitsPlayer(RE::TESObjectARMO* a_armo) {
            return EvaluateFit(a_armo) == FitReason::kFits;
        }

        // Human-readable reason for the flag tooltip, e.g. "no Nord UBE
        // armature" or "no female mesh" - sourced from whichever target the
        // fit cache currently reflects (see RefreshFitFor): the player by
        // default, or the editor's "Editing:" NPC target right after a
        // switch. Never null; empty-ish for kFits.
        [[nodiscard]] const char*        FitRaceName() const;
        [[nodiscard]] static std::string FitReasonText(FitReason a_reason);

        // Sex index (RE::SEXES::kMale/kFemale) for an arbitrary base NPC
        // record, same "GetSex()==kFemale, else male" convention as the
        // player's own derivation (see PlayerSexIdx in the .cpp). Lets the
        // editor's target-switch code (Task 8) share one sex derivation
        // instead of re-deriving it inline at the RefreshFitFor call site.
        [[nodiscard]] static int SexIdxOf(RE::TESNPC* a_npc);

        // Items whose PRIMARY slot is the given bit (multi-slot armor lists
        // under its lowest slot only - no duplicates across slots), filtered
        // by a case-insensitive substring of the name OR source plugin (empty
        // = all). a_collectedOnly additionally limits to looks in the player's
        // Collection (owned-at-some-point); a_armorType (0 light, 1 heavy,
        // 2 clothing; -1 = any) narrows to one armor class; a_favoritesOnly
        // limits to starred looks (OS-22). Styles from newly-added plugins are
        // never filtered - they just sort to the top and carry a NEW badge
        // (OS-26). Unfit styles are never hidden either - they render red (see
        // fitReason) so nothing silently vanishes from the catalog.
        //
        // a_weaponClass switches the DIMENSION: nullopt (the default) is an
        // ARMOR query - a_bit/a_armorType apply and only armor styles come
        // back; set is a WEAPON query - only WEAP/AMMO styles of that class
        // come back, and a_bit/a_armorType are ignored (a weapon has neither).
        [[nodiscard]] std::vector<const StyleItem*> Query(
            std::uint32_t a_bit, std::string_view a_search, bool a_collectedOnly, int a_armorType,
            bool a_favoritesOnly, std::optional<WeaponClass> a_weaponClass = std::nullopt) const;

        // Bitmask of primary slots that have at least one item matching the
        // search - drives the slot-list highlight while searching. ARMOR only
        // (it returns armor slot bits), so weapon styles never light a bit.
        //
        // ⚠⚠ a_hideUnfit MUST BE THE BROWSER'S OWN SETTING. Gold promises
        // "click here and it is in there", so this has to ask the question the
        // LIST asks, not a looser one: the browser drops body-unfit rows when
        // bBrowserHideUnfit is on (its default), and a mask that counted them
        // lit a slot whose pane then opened empty (field 2026-08-27). This is
        // the same rule HeadListHasSearchHit carries, on the dimension that
        // never got it.
        [[nodiscard]] std::uint32_t MatchMask(std::string_view a_search, bool a_collectedOnly,
                                              int a_armorType, bool a_favoritesOnly,
                                              bool a_hideUnfit) const;

        [[nodiscard]] std::size_t Size() const { return items_.size(); }

        // Has the player owned ANY record of the look a_formID belongs to?
        // a_formID may be the representative or any collapsed variant. Forms
        // outside the catalog fall back to a plain Collection lookup, so a
        // preset naming a piece the catalog dropped still answers honestly.
        // The one collection question callers outside the browser should ask -
        // Collection::Knows alone is record-scoped and under-reports (see
        // StyleItem::variantIds).
        [[nodiscard]] bool IsLookCollected(RE::FormID a_formID) const;

        // All indexed items - read on the main thread (auto-preset generation
        // runs there, same discipline as Query). Do not hold across a Build().
        [[nodiscard]] const std::vector<StyleItem>& Items() const { return items_; }

    private:
        StyleCatalog() = default;
        std::vector<StyleItem> items_;

        // Every form the catalog knows - representatives AND the variants
        // collapsed into them - mapped to its look's index in items_. Built
        // at the end of Build(), after items_ has settled, so the indices
        // cannot go stale within a build.
        std::unordered_map<RE::FormID, std::size_t> lookIndex_;

        // The fit cache's current SUBJECT: whose race+sex fitsBody/fitReason
        // were last evaluated against. Player by default; an NPC target
        // after RefreshFitFor(). fitSubjectIsPlayer_ is the authoritative
        // "is it the player" bit EnsureFitCurrent gates on - it is set
        // inside RefreshFitFor() by comparing (a_race, a_sexIdx) against the
        // player's OWN live race+sex at call time, not by comparing fitRace_
        // to the player's CURRENT race (that comparison cannot tell "the
        // player's race changed since we cached" apart from "the cache
        // subject is a different-race NPC" - both look like a mismatch).
        RE::TESRace* fitRace_{ nullptr };
        int          fitSexIdx_{ RE::SEXES::kMale };
        bool         fitSubjectIsPlayer_{ true };
    };

    // ---- the newly-found mark, asked of the whole collapsed look ----------
    //
    // ⚠⚠ THEY LIVE BESIDE LookCollected FOR THE REASON StyleItem::variantIds
    // ALREADY GIVES. Ownership is recorded per RECORD, and one row can stand
    // for a dozen of them, so asking Collection::Knows about the representative
    // alone is the bug that once hid whole looks from the browser. "New" has to
    // answer over the same set "collected" does, or a row could be offered as
    // owned and marked as never seen at the same time.
    //
    // ⚠ NEW MEANS COLLECTED AND NOT ACKNOWLEDGED. A look the player has never
    // owned is not new, it is absent, and with the collection filter on it is
    // not even drawn.
    [[nodiscard]] bool LookIsNew(const StyleItem& a_item);

    // Mark every record of this look that the player actually owns. Marking
    // only the representative would leave a row that came in as a variant
    // permanently new, because LookIsNew reads the same set both ways.
    void AcknowledgeLook(const StyleItem& a_item);

}  // namespace OS
