#include "BodySlideCatalog.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
    int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL " << __LINE__ << ": " #x "\n"; ++failures; } } while (false)
    void Write(const std::filesystem::path& p, const char* text) {
        std::filesystem::create_directories(p.parent_path());
        std::ofstream(p) << text;
    }
}

int main() {
    using namespace OS;
    const auto root = std::filesystem::current_path() / "body-slide-catalog-tests-tmp";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    Write(root / "Config.xml", R"(<Config><Input><SliderMinimum>-100</SliderMinimum><SliderMaximum>200</SliderMaximum></Input></Config>)");
    Write(root / "SliderCategories/3BA.xml", R"(<SliderCategories><Category name="Torso"><Slider name="Waist" displayname="Waist Width"/><Slider name="OnlyProject" displayname="Project Zero"/></Category></SliderCategories>)");
    Write(root / "SliderPresets/body.xml", R"(<SliderPresets><Preset name="Installed" set="CBBE 3BBB Body Amazing"><Group name="3BA"/><SetSlider name="Waist" size="small" value="20"/><SetSlider name="Waist" size="big" value="80"/></Preset></SliderPresets>)");
    Write(root / "SliderSets/body.osp", R"(<SliderSetInfo><SliderSet name="CBBE 3BBB Body Amazing"><OutputPath>meshes\actors\character\character assets</OutputPath><OutputFile GenWeights="true">femalebody</OutputFile><Slider name="Waist"><Data/></Slider><Slider name="OnlyProject"><Data/></Slider><Slider name="UvShape" uv="true"><Data/></Slider><Slider name="Zap" zap="true"><Data/></Slider></SliderSet></SliderSetInfo>)");

    const auto scan = ScanBodySlideCatalog(root, { "Installed", "Missing" });
    CHECK(scan.sliderMinimum == -100.0f);
    CHECK(scan.sliderMaximum == 200.0f);
    CHECK(scan.presets.size() == 2);
    const auto* installed = scan.Find("Installed");
    CHECK(installed != nullptr);
    CHECK(installed->authorable);
    CHECK(installed->seed.family == BodyFamily::k3BA);
    CHECK(installed->seed.sex == BodySex::kFemale);
    CHECK(installed->seed.sliders.size() == 2);
    CHECK(installed->seed.sliders[0].displayName == "Waist Width");
    CHECK(installed->seed.sliders[0].smallValue == 20.0f);
    CHECK(installed->seed.sliders[0].bigValue == 80.0f);
    CHECK(installed->seed.sliders[1].smallValue == 0.0f);
    CHECK(scan.excludedUvSliders == 1);
    CHECK(scan.Find("Missing") != nullptr);
    CHECK(!scan.Find("Missing")->authorable);
    // ⚠ THE ONLY COUNT ABOVE IS TAUTOLOGICAL. presets.size() == 2 because the
    // allowlist held two names and the scan seeds one row per allowed name
    // whatever it finds, so that assertion passes on a scan that read nothing.
    // The drop counters below are what say which of the two arrived how.
    CHECK(scan.drops.noXmlMatch == 1);
    CHECK(scan.drops.setNotInstalled == 0);

    std::filesystem::remove_all(root, ec);
    CHECK(!ec);

    // ---- a set= that names a SliderGroup rather than a slider set ----------
    //
    // ⚠⚠ THIS IS WHERE HALF A LIBRARY WENT. HIMBO's own shipped presets carry
    // set="HIMBO" and no .osp anywhere declares <SliderSet name="HIMBO">; it is
    // a group of hundreds of members. Nothing in the plugin opened a
    // SliderGroups file until 2026-08-29, so every one of those presets was
    // dropped with a clean log.
    {
        const auto groupRoot = std::filesystem::current_path() / "body-slide-group-tests-tmp";
        std::filesystem::remove_all(groupRoot, ec);
        Write(groupRoot / "SliderPresets/himbo.xml",
              R"(<SliderPresets><Preset name="HIMBO Daddy" set="HIMBO"><SetSlider name="Chest" size="small" value="10"/><SetSlider name="Chest" size="big" value="90"/></Preset></SliderPresets>)");
        Write(groupRoot / "SliderGroups/himbo.xml",
              R"(<SliderGroups><Group name="HIMBO"><Member name="HIMBO Body - SOS"/><Member name="HIMBO Boots"/></Group></SliderGroups>)");
        Write(groupRoot / "SliderSets/himbo.osp",
              R"(<SliderSetInfo><SliderSet name="HIMBO Body - SOS"><OutputPath>meshes\actors\character\character assets</OutputPath><OutputFile GenWeights="true">malebody</OutputFile><Slider name="Chest"><Data/></Slider></SliderSet><SliderSet name="HIMBO Boots"><OutputFile>boots</OutputFile><Slider name="Sole"><Data/></Slider></SliderSet></SliderSetInfo>)");

        const auto viaGroup = ScanBodySlideCatalog(groupRoot, { "HIMBO Daddy" });
        CHECK(viaGroup.groupFilesScanned == 1);
        CHECK(viaGroup.drops.setViaGroup == 1);
        CHECK(viaGroup.drops.setNotInstalled == 0);
        const auto* daddy = viaGroup.Find("HIMBO Daddy");
        CHECK(daddy != nullptr);
        CHECK(daddy->authorable);
        CHECK(daddy->seed.family == BodyFamily::kHIMBO);
        CHECK(daddy->seed.sex == BodySex::kMale);
        // ⚠ THE SET THE PRESET DECLARED IS KEPT, not the member it resolved to.
        // seed.sourceSet is written to disk with custom presets, so rewriting it
        // here would change what a stored preset round-trips to.
        CHECK(daddy->seed.sourceSet == "HIMBO");
        CHECK(daddy->outputMesh == "actors\\character\\character assets\\malebody");
        // A member sharing no slider with the preset is not evidence, so the
        // boots must not have won: a group is a bag of every conversion any mod
        // ever added to it.
        CHECK(daddy->seed.sliders.size() == 1);
        CHECK(daddy->seed.sliders[0].name == "Chest");

        std::filesystem::remove_all(groupRoot, ec);
    }

    // ---- a group member that is armour must not win over the body ----------
    //
    // ⚠⚠ THE FIRST CUT OF THE GROUP RESCUE PICKED A SUIT OF ARMOUR, and no
    // fixture caught it because every fixture had one plausible member. On the
    // reference load order the HIMBO group holds "HIMBO Dawnguard - Body -
    // Dawnguard 01", which sets GenWeights, carries the SAME 126 sliders as the
    // real body set, and has "body" in its output filename ("dawnguardbody1m"),
    // so it tied with "HIMBO Body - SOS" on every signal the scan had. What
    // separates them is the output PATH: a character's own body comes from
    // Actor::GetSkin() and lives in the vanilla assets folder, and nothing worn
    // over it does.
    {
        const auto armourRoot = std::filesystem::current_path() / "body-slide-armour-tests-tmp";
        std::filesystem::remove_all(armourRoot, ec);
        Write(armourRoot / "SliderPresets/himbo.xml",
              R"(<SliderPresets><Preset name="HIMBO Hassan" set="HIMBO"><SetSlider name="Chest" size="small" value="10"/><SetSlider name="Chest" size="big" value="90"/></Preset></SliderPresets>)");
        Write(armourRoot / "SliderGroups/himbo.xml",
              R"(<SliderGroups><Group name="HIMBO"><Member name="HIMBO Dawnguard - Body - Dawnguard 01"/><Member name="HIMBO Body - SOS"/></Group></SliderGroups>)");
        // The armour is listed FIRST and is identical in every other field, so
        // a rule that does not read the path picks it.
        Write(armourRoot / "SliderSets/himbo.osp",
              R"(<SliderSetInfo><SliderSet name="HIMBO Dawnguard - Body - Dawnguard 01"><OutputPath>meshes\dlc01\armor\dawnguard</OutputPath><OutputFile GenWeights="true">dawnguardbody1m</OutputFile><Slider name="Chest"><Data/></Slider></SliderSet><SliderSet name="HIMBO Body - SOS"><OutputPath>meshes\actors\character\character assets</OutputPath><OutputFile GenWeights="true">malebody</OutputFile><Slider name="Chest"><Data/></Slider></SliderSet></SliderSetInfo>)");

        const auto armour = ScanBodySlideCatalog(armourRoot, { "HIMBO Hassan" });
        CHECK(armour.drops.setViaGroup == 1);
        const auto* hassan = armour.Find("HIMBO Hassan");
        CHECK(hassan != nullptr);
        CHECK(hassan->authorable);
        CHECK(hassan->outputMesh == "actors\\character\\character assets\\malebody");

        std::filesystem::remove_all(armourRoot, ec);
    }

    // ---- the scan no longer censors, and no longer stops at one level ------
    {
        const auto openRoot = std::filesystem::current_path() / "body-slide-open-tests-tmp";
        std::filesystem::remove_all(openRoot, ec);
        // A subfolder. BodySlide walks these and this plugin's own exports go
        // to SliderPresets\Fitting Room, so a flat walk could not see the files
        // this mod writes.
        Write(openRoot / "SliderPresets/Author Pack/deep.xml",
              R"(<SliderPresets><Preset name="Nested Body" set="CBBE Body"><SetSlider name="Waist" size="small" value="5"/><SetSlider name="Waist" size="big" value="55"/></Preset></SliderPresets>)");
        // A filename carrying "outfit" used to skip the WHOLE file, and a preset
        // name carrying "push" or "cloth" used to skip that preset. Both are
        // body presets the player chose in BodySlide.
        Write(openRoot / "SliderPresets/outfit pack.xml",
              // ⚠ A CUSTOM DELIMITER, because the preset name ends in ")" and
              // the attribute quote follows it, which closes a plain R"( ... )".
              R"XML(<SliderPresets><Preset name="Pushup Curves (Clothes)" set="CBBE Body"><SetSlider name="Waist" size="small" value="7"/><SetSlider name="Waist" size="big" value="77"/></Preset></SliderPresets>)XML");
        Write(openRoot / "SliderSets/cbbe.osp",
              R"(<SliderSetInfo><SliderSet name="CBBE Body"><OutputPath>meshes\actors\character\character assets</OutputPath><OutputFile GenWeights="true">femalebody</OutputFile><Slider name="Waist"><Data/></Slider></SliderSet></SliderSetInfo>)");

        // ⚠ THE EMPTY ALLOWLIST PATH, WHICH NO TEST HAD EVER RUN. Every case
        // above hands ScanBodySlideCatalog a roster, so the branch that
        // enumerates what is actually on disk was untested, and it is the only
        // branch whose count is not decided by its own argument.
        const auto open = ScanBodySlideCatalog(openRoot);
        CHECK(open.presetFilesScanned == 2);
        CHECK(open.presets.size() == 2);
        CHECK(open.drops.notInRoster == 0);
        const auto* nested = open.Find("Nested Body");
        CHECK(nested != nullptr && nested->authorable);
        const auto* pushup = open.Find("Pushup Curves (Clothes)");
        CHECK(pushup != nullptr && pushup->authorable);

        // The roster is OBody's, and a preset it does not name cannot appear
        // however well the scan read it. Counted so a log can say so.
        const auto narrowed = ScanBodySlideCatalog(openRoot, { "Nested Body" });
        CHECK(narrowed.presets.size() == 1);
        CHECK(narrowed.drops.notInRoster == 1);

        // One set builds the mesh, so both questions have an answer.
        CHECK(open.FamilyForMesh("actors\\character\\character assets\\femalebody") ==
              BodyFamily::kCBBE);
        CHECK(open.SetForMesh("actors\\character\\character assets\\femalebody") ==
              "CBBE Body");

        std::filesystem::remove_all(openRoot, ec);
    }

    // ---- a mesh two families share cannot name a family --------------------
    //
    // ⚠⚠ THE COIN-FLIP THIS REPLACED. FamilyForMesh used to walk the
    // name-sorted preset list and return the first family it found for the
    // mesh, so one preset's NAME decided what body the character was on. CBBE
    // and 3BA both build femalebody, so whichever sorted first won and the fit
    // filter hid every preset of the other family as a proven misfit.
    {
        const auto sharedRoot = std::filesystem::current_path() / "body-slide-shared-tests-tmp";
        std::filesystem::remove_all(sharedRoot, ec);
        Write(sharedRoot / "SliderPresets/both.xml",
              R"(<SliderPresets><Preset name="- Zeroed Sliders -" set="CBBE Body"><SetSlider name="Waist" size="small" value="0"/><SetSlider name="Waist" size="big" value="0"/></Preset><Preset name="Rugged" set="CBBE 3BBB Body Amazing"><SetSlider name="Waist" size="small" value="30"/><SetSlider name="Waist" size="big" value="70"/></Preset></SliderPresets>)");
        Write(sharedRoot / "SliderSets/both.osp",
              R"(<SliderSetInfo><SliderSet name="CBBE Body"><OutputPath>meshes\actors\character\character assets</OutputPath><OutputFile GenWeights="true">femalebody</OutputFile><Slider name="Waist"><Data/></Slider></SliderSet><SliderSet name="CBBE 3BBB Body Amazing"><OutputPath>meshes\actors\character\character assets</OutputPath><OutputFile GenWeights="true">femalebody</OutputFile><Slider name="Waist"><Data/></Slider></SliderSet></SliderSetInfo>)");

        const auto shared = ScanBodySlideCatalog(sharedRoot);
        CHECK(shared.presets.size() == 2);
        CHECK(shared.FamilyForMesh("actors\\character\\character assets\\femalebody") ==
              BodyFamily::kUnknown);
        CHECK(shared.SetForMesh("actors\\character\\character assets\\femalebody").empty());

        std::filesystem::remove_all(sharedRoot, ec);
    }

    // ---- a score tie whose members agree is not ambiguity -------------------
    //
    // ⚠ A body ships several variants of one set carrying the same sliders, the
    // same family and the same output mesh, so which one wins cannot change
    // anything the preset is built from. Refusing every tie outright cost four
    // presets on the reference load order and would have thrown away every
    // group rescue as well.
    {
        const auto tieRoot = std::filesystem::current_path() / "body-slide-tie-tests-tmp";
        std::filesystem::remove_all(tieRoot, ec);
        Write(tieRoot / "SliderPresets/tie.xml",
              R"(<SliderPresets><Preset name="Variant" set="HIMBO Body - SOS"><SetSlider name="Chest" size="small" value="10"/><SetSlider name="Chest" size="big" value="90"/></Preset></SliderPresets>)");
        // Two files declaring the same set name, identical in every field the
        // tie test consults.
        Write(tieRoot / "SliderSets/a.osp",
              R"(<SliderSetInfo><SliderSet name="HIMBO Body - SOS"><OutputPath>meshes\actors\character\character assets</OutputPath><OutputFile GenWeights="true">malebody</OutputFile><Slider name="Chest"><Data/></Slider></SliderSet></SliderSetInfo>)");
        Write(tieRoot / "SliderSets/b.osp",
              R"(<SliderSetInfo><SliderSet name="HIMBO Body - SOS"><OutputPath>meshes\actors\character\character assets</OutputPath><OutputFile GenWeights="true">malebody</OutputFile><Slider name="Chest"><Data/></Slider></SliderSet></SliderSetInfo>)");

        const auto tie = ScanBodySlideCatalog(tieRoot, { "Variant" });
        CHECK(tie.drops.tiedProjects == 0);
        const auto* variant = tie.Find("Variant");
        CHECK(variant != nullptr && variant->authorable && !variant->ambiguous);

        // Now make them disagree about the mesh they build, which is real
        // ambiguity and must still be refused.
        Write(tieRoot / "SliderSets/b.osp",
              R"(<SliderSetInfo><SliderSet name="HIMBO Body - SOS"><OutputPath>meshes\actors\character\character assets</OutputPath><OutputFile GenWeights="true">malebodyalt</OutputFile><Slider name="Chest"><Data/></Slider></SliderSet></SliderSetInfo>)");
        const auto split = ScanBodySlideCatalog(tieRoot, { "Variant" });
        CHECK(split.drops.tiedProjects == 1);
        CHECK(split.Find("Variant") != nullptr && split.Find("Variant")->ambiguous);

        std::filesystem::remove_all(tieRoot, ec);
    }

    // ---- two mods shipping the same preset is not ambiguity -----------------
    //
    // ⚠⚠ FIELD, 2026-08-29: `HIMBO Five` drew a crossed-out card reading "this
    // preset carries no slider values" because HIMBO's own `HIMBO.xml` and a
    // `HIMBO Five.xml` beside it both declare it, on the same set, with the same
    // 57 sliders. Refusing on the COUNT rather than on a disagreement is what
    // did that, and it is the same mistake ChooseCandidate already avoids for
    // tied projects.
    {
        const auto dupRoot = std::filesystem::current_path() / "body-slide-dup-tests-tmp";
        std::filesystem::remove_all(dupRoot, ec);
        Write(dupRoot / "SliderPresets/pack-a.xml",
              R"(<SliderPresets><Preset name="HIMBO Five" set="HIMBO Body - SOS"><SetSlider name="Chest" size="small" value="10"/><SetSlider name="Chest" size="big" value="90"/></Preset></SliderPresets>)");
        Write(dupRoot / "SliderPresets/pack-b.xml",
              R"(<SliderPresets><Preset name="HIMBO Five" set="HIMBO Body - SOS"><SetSlider name="Chest" size="small" value="10"/><SetSlider name="Chest" size="big" value="90"/></Preset></SliderPresets>)");
        Write(dupRoot / "SliderSets/himbo.osp",
              R"(<SliderSetInfo><SliderSet name="HIMBO Body - SOS"><OutputPath>meshes\actors\character\character assets</OutputPath><OutputFile GenWeights="true">malebody</OutputFile><Slider name="Chest"><Data/></Slider></SliderSet></SliderSetInfo>)");

        const auto agreeing = ScanBodySlideCatalog(dupRoot, { "HIMBO Five" });
        CHECK(agreeing.drops.duplicateName == 0);
        const auto* five = agreeing.Find("HIMBO Five");
        CHECK(five != nullptr);
        CHECK(!five->ambiguous);
        CHECK(five->authorable);
        CHECK(five->seed.sliders.size() == 1);

        // Make the second copy disagree about a slider VALUE. That is real
        // ambiguity and must still be refused.
        Write(dupRoot / "SliderPresets/pack-b.xml",
              R"(<SliderPresets><Preset name="HIMBO Five" set="HIMBO Body - SOS"><SetSlider name="Chest" size="small" value="40"/><SetSlider name="Chest" size="big" value="90"/></Preset></SliderPresets>)");
        const auto conflicting = ScanBodySlideCatalog(dupRoot, { "HIMBO Five" });
        CHECK(conflicting.drops.duplicateName == 1);
        CHECK(conflicting.Find("HIMBO Five") != nullptr &&
              conflicting.Find("HIMBO Five")->ambiguous);

        // And about the SET, which is the other half of the agreement test.
        Write(dupRoot / "SliderPresets/pack-b.xml",
              R"(<SliderPresets><Preset name="HIMBO Five" set="HIMBO Body - SOS High Poly"><SetSlider name="Chest" size="small" value="10"/><SetSlider name="Chest" size="big" value="90"/></Preset></SliderPresets>)");
        const auto splitSet = ScanBodySlideCatalog(dupRoot, { "HIMBO Five" });
        CHECK(splitSet.drops.duplicateName == 1);

        std::filesystem::remove_all(dupRoot, ec);
    }

    // ---- a set that resolves to nothing is still authorable ----------------
    //
    // ⚠⚠ item.authorable WAS NEVER ASSIGNED ON THIS PATH. The branch filled the
    // family, the sex and every slider and then pushed the row with authorable
    // left at its default false, and the Body Studio card grid skips a row it
    // cannot open, so the preset vanished rather than appearing unbuilt.
    {
        const auto orphanRoot = std::filesystem::current_path() / "body-slide-orphan-tests-tmp";
        std::filesystem::remove_all(orphanRoot, ec);
        Write(orphanRoot / "SliderPresets/orphan.xml",
              R"(<SliderPresets><Preset name="Orphan" set="UBE SE 2.0 Release Body"><SetSlider name="Waist" size="small" value="12"/><SetSlider name="Waist" size="big" value="88"/></Preset></SliderPresets>)");

        const auto orphan = ScanBodySlideCatalog(orphanRoot, { "Orphan" });
        CHECK(orphan.drops.setNotInstalled == 1);
        const auto* row = orphan.Find("Orphan");
        CHECK(row != nullptr);
        CHECK(row->seed.family == BodyFamily::kUBE);
        CHECK(row->seed.sex == BodySex::kFemale);
        CHECK(row->seed.sliders.size() == 1);
        CHECK(row->authorable);
        CHECK(row->outputMesh.empty());

        std::filesystem::remove_all(orphanRoot, ec);
    }
    if (!failures) std::cout << "BodySlideCatalogTests: all passed\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
