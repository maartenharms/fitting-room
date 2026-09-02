#pragma once

#include "BodyPreset.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace OS {

    struct BodyCatalogPreset {
        BodyPreset seed;
        bool       authorable{ false };
        bool       ambiguous{ false };
        // The mesh this preset's slider set builds, as a BodyMeshKey. Empty
        // when the project could not be read.
        //
        // ⚠ HERE AND NOT ON BodyPreset, DELIBERATELY. BodyPreset is written to
        // disk as JSON by BodyPresetStore, so a field on it is a format change
        // every stored preset has to survive. This is derived from the install
        // and belongs to the scan that read it. A custom preset's mesh is found
        // by looking its sourceSet up here, which is MeshForSet below.
        std::string outputMesh;
        // Whether that mesh is built as the _0 / _1 pair or as a bare .nif.
        // Pairs with outputMesh through BodyMeshModelPath (BodyMeshPath.h):
        // the key throws the suffix away and a card has to put it back.
        bool        genWeights{ false };
        std::string diagnostic;
        std::filesystem::path presetFile;
        std::filesystem::path projectFile;
    };

    // Why a preset that exists on disk did not reach the player, counted per
    // reason.
    //
    // ⚠⚠ THE SCAN CARRIED NO LOGGING AT ALL UNTIL 2026-08-29 AND THAT IS WHY
    // A REPORT OF "not all bodyslide presets show up" COULD NOT BE ANSWERED.
    // Six rules can drop a preset and the only number the snapshot published
    // was a total, which every one of them moves in the same direction. A
    // count per reason is what tells a set the player never installed apart
    // from a name we refused to read.
    struct BodySlideDropCounts {
        std::size_t missingNameOrSet{ 0 };  // no name= or an empty set=
        std::size_t oversizeName{ 0 };      // a name or set over the byte cap
        std::size_t malformedSlider{ 0 };   // a SetSlider row would not parse
        std::size_t notInRoster{ 0 };       // on disk, but OBody never named it
        std::size_t noXmlMatch{ 0 };        // OBody named it, no XML has it
        std::size_t duplicateName{ 0 };     // the name is in two XML entries
        std::size_t setNotInstalled{ 0 };   // set= names no .osp we can see
        std::size_t setViaGroup{ 0 };       // ...but a SliderGroup rescued it
        std::size_t tiedProjects{ 0 };      // two .osp score identically
    };

    struct BodySlideCatalogSnapshot {
        std::vector<BodyCatalogPreset> presets;
        float                          sliderMinimum{ 0.0f };
        float                          sliderMaximum{ 100.0f };
        std::size_t                    presetFilesScanned{ 0 };
        std::size_t                    projectFilesScanned{ 0 };
        std::size_t                    categoryFilesScanned{ 0 };
        std::size_t                    groupFilesScanned{ 0 };
        std::size_t                    rejectedFiles{ 0 };
        std::size_t                    excludedUvSliders{ 0 };
        BodySlideDropCounts            drops;
        std::string                    diagnostic;
        std::uint64_t                  generation{ 0 };

        // Every slider set any scanned .osp declares, mapped to that file.
        //
        // ⚠⚠ IT COVERS SETS NO INSTALLED PRESET USES, which is the whole
        // reason it exists. A body card finds its .osp through the catalog,
        // and the candidate machinery below only records projects for sets an
        // installed preset asked for. A set used ONLY by custom presets was
        // therefore unreachable and every preset on it drew a cross with a
        // clean log: HIMBO and four variants, UBE 3.0 Release Preview, UBE 2.0
        // Yarin Chest and UBE 2.0 [Melodic] WL Boots, all in one field session
        // (2026-08-10).
        //
        // ⚠ FIRST FILE WINS when two .osp files declare the same set name.
        // That is a simplification rather than a rule: the reader validates
        // the set against the file it is handed and warns when it is not
        // there, so a wrong pick is loud instead of silent.
        std::unordered_map<std::string, std::string> projectForSet;

        // Every declared set's <OutputFile>, on the same terms as
        // projectForSet and for the same reason: the COVERED sibling of a body
        // set almost never has an installed preset of its own, so nothing
        // derived from the preset list can see it. This is what tells a
        // femalebody set from a maleunderwear one without reopening the .osp.
        std::unordered_map<std::string, std::string> outputForSet;

        // The same coverage as outputForSet, but as a BodyMeshKey rather than
        // the bare <OutputFile> text.
        //
        // ⚠ A SECOND MAP RATHER THAN A CHANGED VALUE. outputForSet's consumer
        // (BodyCardScene's covered-sibling search) wants the filename as
        // written; FamilyForMesh and SetForMesh want it joined to <OutputPath>
        // and normalised, which is a different string. One map cannot be both
        // and the difference is silent: the raw filename never equals a mesh
        // key, so a comparison against the wrong one simply never matches.
        std::unordered_map<std::string, std::string> meshForDeclaredSet;

        [[nodiscard]] const BodyCatalogPreset* Find(std::string_view a_name) const;

        // The mesh built by any preset using this slider set, or empty. Custom
        // presets carry a sourceSet and no mesh of their own, so this is how
        // they are placed on the same footing as an installed one.
        [[nodiscard]] std::string_view MeshForSet(std::string_view a_set) const;

        // The family of the body that builds this mesh, so the character's own
        // body can be classified the same way its presets are. Unknown when
        // nothing builds it AND when the sets that do build it disagree.
        //
        // ⚠⚠ IT ASKS EVERY DECLARED SET, NOT THE FIRST PRESET BY NAME, AND A
        // DISAGREEMENT ANSWERS UNKNOWN. The first cut walked the name-sorted
        // preset list and returned the first family it found for the mesh,
        // which made one preset's NAME decide what body the character was on.
        // CBBE and 3BA both build actors\character\character assets\femalebody,
        // so on the ordinary CBBE+3BA install "- Zeroed Sliders -" sorted first,
        // the body was declared CBBE, and the fit filter then hid all sixty 3BA
        // presets as proven misfits. Deleting that one preset flipped the
        // verdict and hid the fifteen CBBE ones instead. A mesh two families
        // share cannot name a family, and saying so lets the filter fail open
        // where it has no evidence, which is what BodyFitCompatible documents.
        [[nodiscard]] BodyFamily FamilyForMesh(std::string_view a_mesh) const;

        // The slider set that builds this mesh, when exactly one declared set
        // does. Empty when none does or several do, on the same terms as
        // FamilyForMesh and for the same reason: it feeds BuiltBody::sourceSet,
        // whose only job is to prove a fit, so an ambiguous answer must not be
        // guessed at.
        [[nodiscard]] std::string_view SetForMesh(std::string_view a_mesh) const;
    };

    // Which body family a slider set name belongs to.
    //
    [[nodiscard]] BodySlideCatalogSnapshot ScanBodySlideCatalog(
        const std::filesystem::path& a_bodySlideRoot,
        const std::vector<std::string>& a_allowedPresetNames = {});

    class BodySlideCatalog {
    public:
        static BodySlideCatalog& GetSingleton();

        void RequestScan(std::vector<std::string> a_allowedPresetNames);
        [[nodiscard]] std::shared_ptr<const BodySlideCatalogSnapshot> Snapshot() const;
        [[nodiscard]] bool Scanning() const;

    private:
        BodySlideCatalog();

        std::atomic<std::shared_ptr<const BodySlideCatalogSnapshot>> snapshot_;
        std::atomic<std::uint32_t> activeScans_{ 0 };
        std::atomic<std::uint64_t> generation_{ 0 };
    };

}  // namespace OS
