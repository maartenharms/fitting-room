// Pure-logic tests for reading the installed body's slider vocabulary
// (BodyMorphCatalogParse.h). No engine, no filesystem, no XML library: the
// input is a string and the output is what the Shape page draws.
//
// Three cases carry this file, and every one of them was measured off the
// reference load order rather than imagined.
//
// The malformed attribute is the first. Caliente's own shipped CBBE.xml
// contains `name="Hips"displayname="Size"` with no space between the
// attributes. A conforming parser rejects the document, and rejecting it drops
// all thirteen CBBE categories at once, which on screen looks like "this body
// has no sliders" rather than like a parse error.
//
// The entity is the second. UBE ships slider names carrying &gt;, and the name
// SetMorph wants is the decoded one, so a page built on the raw text would push
// morph names no body has and move nothing.
//
// The filter is the third. Five bodies' category files are installed side by
// side here, and a morph name the built body lacks is stored by RaceMenu
// without complaint and does nothing at all. A control that silently does
// nothing is the failure this whole file exists to prevent.
#include "BodyMorphCatalogParse.h"

#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

namespace {

    using namespace OS::BodyMorphCatalogParse;

    [[nodiscard]] const Category* Find(const std::vector<Category>& a_cats,
                                       std::string_view              a_name) {
        for (const auto& c : a_cats) {
            if (c.name == a_name) {
                return &c;
            }
        }
        return nullptr;
    }

    [[nodiscard]] const SliderEntry* FindSlider(const std::vector<Category>& a_cats,
                                                std::string_view              a_name) {
        for (const auto& c : a_cats) {
            for (const auto& s : c.sliders) {
                if (s.name == a_name) {
                    return &s;
                }
            }
        }
        return nullptr;
    }

}  // namespace

int main() {
    {  // The ordinary shape, straight out of CBBE.xml.
        const std::string_view xml = R"(<?xml version="1.0"?>
<SliderCategories>
  <Category name="Breasts" defaultHidden="false">
    <Slider name="Breasts" displayname="Size" />
    <Slider name="BreastsSmall" displayname="Smaller 1" />
  </Category>
  <Category name="Muscle Definition">
    <Slider name="MuscleAbs" displayname="Abs" />
  </Category>
</SliderCategories>)";
        const auto cats = ParseCategories(xml);
        CHECK(cats.size() == 2);
        CHECK(Find(cats, "Breasts") && Find(cats, "Breasts")->sliders.size() == 2);
        CHECK(Find(cats, "Breasts")->sliders[0].display == "Size");
        CHECK(Find(cats, "Breasts")->sliders[0].name == "Breasts");
        CHECK(Find(cats, "Muscle Definition")->sliders[0].display == "Abs");
        CHECK(!Find(cats, "Breasts")->defaultHidden);
        CHECK(CountSliders(cats) == 3);
    }

    {  // ⚠ THE MALFORMED ATTRIBUTE, copied from the shipped CBBE.xml. The
       // missing space costs that attribute at worst, never the file.
        const std::string_view xml = R"(<SliderCategories>
  <Category name="Hips">
    <Slider name="HipBone" displayname="Hip Bone" />
    <Slider name="Hips"displayname="Size"  />
    <Slider name="HipForward" displayname="Forward" />
  </Category>
</SliderCategories>)";
        const auto cats = ParseCategories(xml);
        CHECK(cats.size() == 1);
        CHECK(cats[0].sliders.size() == 3);
        CHECK(FindSlider(cats, "Hips") != nullptr);
        CHECK(FindSlider(cats, "Hips")->display == "Size");
        CHECK(FindSlider(cats, "HipForward")->display == "Forward");
    }

    {  // ⚠ THE ENTITY. UBE's names really do carry these, and the decoded name
       // is the one SetMorph wants.
        const std::string_view xml = R"(<SliderCategories>
  <Category name="Upper Torso">
    <Slider name="NeckSize__HEAD&gt;" displayname="Neck Size" />
    <Slider name="A&amp;B" displayname="A &amp; B" />
  </Category>
</SliderCategories>)";
        const auto cats = ParseCategories(xml);
        CHECK(cats[0].sliders.size() == 2);
        CHECK(FindSlider(cats, "NeckSize__HEAD>") != nullptr);
        CHECK(FindSlider(cats, "A&B") != nullptr);
        CHECK(FindSlider(cats, "A&B")->display == "A & B");
    }

    {  // A name with a > inside it must not end the tag early. This is the same
       // UBE family of names, unencoded, which files in the wild also contain.
        const std::string_view xml = R"(<SliderCategories>
  <Category name="Lower Torso">
    <Slider name="Waist n|p" displayname="Waist Size" />
  </Category>
</SliderCategories>)";
        const auto cats = ParseCategories(xml);
        CHECK(cats.size() == 1);
        CHECK(FindSlider(cats, "Waist n|p")->display == "Waist Size");
    }

    {  // No display name falls back to the raw name, so a control is never
       // drawn blank.
        const auto cats = ParseCategories(
            R"(<SliderCategories><Category name="Butt"><Slider name="RoundAss" /></Category></SliderCategories>)");
        CHECK(cats[0].sliders[0].display == "RoundAss");
    }

    {  // defaultHidden is carried through: it is the body author saying this
       // group starts collapsed, and honouring it is free.
        const auto cats = ParseCategories(
            R"(<SliderCategories><Category name="Hide Thigh" defaultHidden="true"><Slider name="HideThigh" /></Category></SliderCategories>)");
        CHECK(cats.size() == 1);
        CHECK(cats[0].defaultHidden);
    }

    {  // An empty category is a heading with no controls, so it is dropped.
       // A slider outside any category is dropped too: BodySlide would not
       // show it either, and inventing a home for it puts controls on the page
       // the body's own tool hides.
        const auto cats = ParseCategories(
            R"(<SliderCategories><Slider name="Loose" /><Category name="Empty"></Category><Category name="Real"><Slider name="R" /></Category></SliderCategories>)");
        CHECK(cats.size() == 1);
        CHECK(cats[0].name == "Real");
        CHECK(FindSlider(cats, "Loose") == nullptr);
    }

    {  // Comments and the declaration are skipped rather than parsed as tags.
        const auto cats = ParseCategories(
            "<?xml version=\"1.0\"?><!-- <Category name=\"Ghost\"><Slider name=\"G\" /> -->"
            "<SliderCategories><Category name=\"Real\"><Slider name=\"R\" /></Category></SliderCategories>");
        CHECK(cats.size() == 1);
        CHECK(cats[0].name == "Real");
    }

    {  // Junk in, nothing out. None of these may crash or invent a category.
        CHECK(ParseCategories("").empty());
        CHECK(ParseCategories("<").empty());
        CHECK(ParseCategories("<Category name=\"unterminated").empty());
        CHECK(ParseCategories("<!-- never closed").empty());
        CHECK(ParseCategories("<Category><Slider name=\"x\" /></Category>").empty());
        CHECK(ParseCategories("<Category name=\"c\"><Slider /></Category>").empty());
    }

    {  // The slider set: hidden and uv sliders are not body shape and BodySlide
       // draws neither, so neither reaches the page.
        const std::string_view osp = R"(<SliderSetInfo version="1">
  <SliderSet name="UBE SE 2.0 - Necoco Body">
    <Slider name="SkinnyMorph" invert="false" small="100" big="0" hidden="true" />
    <Slider name="NipplesShowUp" invert="false" small="0" big="0" />
    <Slider name="BrowsUV" uv="true" />
    <Slider name="NeckSize__HEAD&gt;" hidden="true" />
    <Slider name="Waist n|p" />
  </SliderSet>
  <SliderSet name="Some Outfit">
    <Slider name="OutfitOnly" />
  </SliderSet>
</SliderSetInfo>)";
        const auto names = ParseSliderSetNames(osp, "UBE SE 2.0 - Necoco Body");
        CHECK(names.size() == 2);
        CHECK(names.contains("NipplesShowUp"));
        CHECK(names.contains("Waist n|p"));
        CHECK(!names.contains("SkinnyMorph"));
        CHECK(!names.contains("BrowsUV"));
        // ⚠ The named set only. Reading a whole file's sliders would drag in
        // every outfit project's names and defeat the filter.
        CHECK(!names.contains("OutfitOnly"));

        const auto all = ParseSliderSetNames(osp, "");
        CHECK(all.contains("OutfitOnly"));
        CHECK(all.contains("NipplesShowUp"));

        CHECK(ParseSliderSetNames(osp, "No Such Set").empty());
    }

    {  // Which set a preset was built against.
        const std::string_view xml = R"(<SliderPresets>
  <Preset name="UBE Necoco V2" set="UBE SE 2.0 - Necoco Body" >
    <SetSlider name="Breasts" size="big" value="70" />
  </Preset>
  <Preset name="Other" set="CBBE 3BBB Body Amazing" />
</SliderPresets>)";
        CHECK(ParsePresetSet(xml, "UBE Necoco V2") == "UBE SE 2.0 - Necoco Body");
        CHECK(ParsePresetSet(xml, "Other") == "CBBE 3BBB Body Amazing");
        CHECK(ParsePresetSet(xml, "Missing").empty());
        CHECK(ParsePresetSet("", "UBE Necoco V2").empty());
    }

    {  // ⚠ THE FILTER. A morph the built body lacks is stored by RaceMenu and
       // moves nothing, so a control for it is worse than no control.
        const std::vector<Category> cats{
            Category{ "Breasts", false, { { "Breasts", "Size" }, { "Ghost", "Ghost" } } },
            Category{ "Nowhere", false, { { "AlsoGhost", "Also" } } },
        };
        const auto kept = KeepAvailable(cats, { "Breasts" });
        CHECK(kept.size() == 1);
        CHECK(kept[0].name == "Breasts");
        CHECK(kept[0].sliders.size() == 1);
        CHECK(kept[0].sliders[0].name == "Breasts");

        // ⚠ An empty available set means the built body could not be worked
        // out, and everything is kept. Too much is recoverable; nothing looks
        // broken.
        CHECK(KeepAvailable(cats, {}).size() == 2);
        CHECK(CountSliders(KeepAvailable(cats, {})) == 3);
    }

    {  // Merging several files: first file wins a slider, so a name two bodies
       // both claim does not appear under two headings.
        const std::vector<Category> ube{
            Category{ "Boobs", false, { { "Breasts", "Bigger" } } },
            Category{ "Hips", false, { { "Hips", "Size" } } },
        };
        const std::vector<Category> cbbe{
            Category{ "Breasts", false, { { "Breasts", "Size" } } },
            Category{ "Hips", false, { { "HipBone", "Hip Bone" } } },
        };
        const auto merged = Merge({ ube, cbbe });
        CHECK(Find(merged, "Boobs")->sliders.size() == 1);
        CHECK(Find(merged, "Breasts") == nullptr);  // its only slider was taken
        CHECK(Find(merged, "Hips")->sliders.size() == 2);
        CHECK(CountSliders(merged) == 3);
        // Order follows first sight, so the body's own file leads the page.
        CHECK(merged[0].name == "Boobs");
    }

    {  // Search reads what the user can see, and the raw name as well: a body
       // that calls six different controls "Size" is otherwise unsearchable.
        const SliderEntry s{ "MuscleAbs", "Abs" };
        CHECK(Matches(s, ""));
        CHECK(Matches(s, "abs"));
        CHECK(Matches(s, "ABS"));
        CHECK(Matches(s, "muscle"));
        CHECK(!Matches(s, "butt"));
    }

    {  // ⚠ THE SAVE FORMAT IS BODYSLIDE'S, WHICH IS ALSO SAM'S. Screen Archer
       // Menu's own BodyMorphs.yaml points its save and load at
       // Data\CalienteTools\BodySlide\SliderPresets with ext .xml, so a file in
       // this shape is readable by SAM, BodySlide and OBody without any of them
       // knowing Fitting Room exists.
        const std::vector<std::pair<std::string, float>> values{
            { "Breasts", 0.7f },
            { "Untouched", 0.0f },
            { "A&B", -0.25f },
        };
        const auto xml = BuildPresetXml("My Shape", "UBE SE 2.0 - Necoco Body", values);

        CHECK(xml.find("<SliderPresets>") != std::string::npos);
        CHECK(xml.find("name=\"My Shape\"") != std::string::npos);
        CHECK(xml.find("set=\"UBE SE 2.0 - Necoco Body\"") != std::string::npos);
        // Percent on disk, fractions in memory.
        CHECK(xml.find("name=\"Breasts\" size=\"small\" value=\"70\"") != std::string::npos);
        CHECK(xml.find("name=\"Breasts\" size=\"big\" value=\"70\"") != std::string::npos);
        CHECK(xml.find("value=\"-25\"") != std::string::npos);
        // ⚠ Zeroes are omitted, or loading the preset in SAM would stamp a zero
        // over every slider the user had not touched.
        CHECK(xml.find("Untouched") == std::string::npos);
        // The name is escaped on the way out and decoded on the way back in.
        CHECK(xml.find("name=\"A&amp;B\"") != std::string::npos);

        // Round trip through our own reader.
        const auto back = ParsePresetSliders(xml, "My Shape");
        CHECK(back.size() == 2);
        CHECK(back[0].first == "Breasts");
        CHECK(back[0].second > 0.699f && back[0].second < 0.701f);
        CHECK(back[1].first == "A&B");
        CHECK(back[1].second < -0.249f && back[1].second > -0.251f);
        CHECK(ParsePresetSliders(xml, "Not This One").empty());
    }

    {  // A foreign preset carrying different endpoints: big wins, small is the
       // fallback, and a preset naming several is not mixed up with its
       // neighbours.
        const std::string_view xml = R"(<SliderPresets>
  <Preset name="Slim" set="CBBE 3BBB Body Amazing">
    <SetSlider name="Breasts" size="small" value="10"/>
    <SetSlider name="Breasts" size="big" value="40"/>
    <SetSlider name="Waist" size="small" value="25"/>
  </Preset>
  <Preset name="Curvy" set="CBBE 3BBB Body Amazing">
    <SetSlider name="Breasts" size="big" value="90"/>
  </Preset>
</SliderPresets>)";
        const auto slim = ParsePresetSliders(xml, "Slim");
        CHECK(slim.size() == 2);
        CHECK(slim[0].first == "Breasts");
        CHECK(slim[0].second > 0.399f && slim[0].second < 0.401f);  // big won
        CHECK(slim[1].second > 0.249f && slim[1].second < 0.251f);  // small only
        const auto curvy = ParsePresetSliders(xml, "Curvy");
        CHECK(curvy.size() == 1);
        CHECK(curvy[0].second > 0.899f);
    }

    {  // A value that is not a number is a corrupt row and costs that row, not
       // the file.
        const std::string_view xml = R"(<SliderPresets><Preset name="P" set="S">
  <SetSlider name="Good" size="big" value="50"/>
  <SetSlider name="Bad" size="big" value="wat"/>
  <SetSlider name="AlsoGood" size="big" value="-30"/>
</Preset></SliderPresets>)";
        const auto p = ParsePresetSliders(xml, "P");
        CHECK(p.size() == 2);
        CHECK(p[0].first == "Good");
        CHECK(p[1].first == "AlsoGood");
    }

    {  // The preset name reaches the filesystem, so it cannot carry a path or
       // anything Windows refuses.
        CHECK(SafeFileStem("My Shape") == "My Shape");
        CHECK(SafeFileStem("..\\..\\evil") == ".._.._evil");
        CHECK(SafeFileStem("a/b:c*d?e\"f<g>h|i") == "a_b_c_d_e_f_g_h_i");
        CHECK(SafeFileStem("trailing dot.") == "trailing dot");
        CHECK(SafeFileStem("  padded  ") == "padded");
        CHECK(SafeFileStem("").empty());
        CHECK(SafeFileStem(std::string(300, 'x')).size() == 96);
    }

    if (g_failures == 0) {
        std::printf("test_bodymorphcatalog: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
