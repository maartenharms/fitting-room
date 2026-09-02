#include "BodyStudioProofData.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {
    int g_failures = 0;

#define CHECK(expr)                                                                         \
    do {                                                                                    \
        if (!(expr)) {                                                                      \
            std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << ": " #expr << '\n'; \
            ++g_failures;                                                                   \
        }                                                                                   \
    } while (false)

    void CheckNear(float a_actual, float a_expected) {
        if (std::fabs(a_actual - a_expected) > 0.00001f) {
            std::cerr << "FAIL: expected " << a_expected << ", got " << a_actual << '\n';
            ++g_failures;
        }
    }

    const OS::BodyStudioProofData::Slider* FindSlider(
        const OS::BodyStudioProofData::Preset& a_preset, std::string_view a_name) {
        for (const auto& slider : a_preset.sliders) {
            if (slider.name == a_name) {
                return &slider;
            }
        }
        return nullptr;
    }

    const OS::RaceMenuMorphApi::MorphValue* FindMorph(
        const std::vector<OS::RaceMenuMorphApi::MorphValue>& a_plan,
        std::string_view a_name) {
        for (const auto& morph : a_plan) {
            if (morph.name == a_name) {
                return &morph;
            }
        }
        return nullptr;
    }
}

int main() {
    using namespace OS::BodyStudioProofData;

    {
        CheckNear(ConvertEndpoint("3BA", "Waist", 150.0f), 1.5f);
        CheckNear(ConvertEndpoint("UBE", "Waist", -20.0f), -0.2f);
        CheckNear(ConvertEndpoint("HIMBO", "PecsMass", 40.0f), 0.4f);

        constexpr std::string_view xml = R"xml(
<SliderPresets>
  <Preset name="Three" set="CBBE 3BBB Body Amazing">
    <Group name="3BA"/>
    <SetSlider name="Waist" size="small" value="20"/>
    <SetSlider name="Waist" size="big" value="80"/>
    <SetSlider name="OnlyBig" size="big" value="50"/>
    <SetSlider name="FirstWins" size="small" value="25"/>
    <SetSlider name="FirstWins" size="small" value="75"/>
    <SetSlider name="ZeroThenValue" size="big" value="0"/>
    <SetSlider name="ZeroThenValue" size="big" value="30"/>
  </Preset>
</SliderPresets>)xml";
        std::string error;
        const auto matches = ParseMatches(xml, "Three", error);
        CHECK(error.empty());
        CHECK(matches.size() == 1);
        CHECK(matches[0].family == Family::k3BA);
        CHECK(matches[0].sourceSet == "CBBE 3BBB Body Amazing");
        const auto* waist = FindSlider(matches[0], "Waist");
        CHECK(waist != nullptr);
        CheckNear(waist->small, 0.2f);
        CheckNear(waist->big, 0.8f);
        const auto* onlyBig = FindSlider(matches[0], "OnlyBig");
        CHECK(onlyBig != nullptr);
        CheckNear(onlyBig->small, 0.0f);
        CheckNear(onlyBig->big, 0.5f);
        CheckNear(FindSlider(matches[0], "FirstWins")->small, 0.25f);
        CheckNear(FindSlider(matches[0], "ZeroThenValue")->big, 0.3f);

        const auto low = BuildPlan(matches[0], 0.0f);
        const auto mid = BuildPlan(matches[0], 50.0f);
        const auto high = BuildPlan(matches[0], 100.0f);
        CheckNear(FindMorph(low, "Waist")->value, 0.2f);
        CheckNear(FindMorph(mid, "Waist")->value, 0.5f);
        CheckNear(FindMorph(high, "Waist")->value, 0.8f);
        CHECK(FindMorph(low, "OnlyBig") == nullptr);  // zero values are not emitted
        CheckNear(FindMorph(mid, "OnlyBig")->value, 0.25f);
    }

    {
        constexpr std::string_view xml = R"xml(
<SliderPresets>
  <Preset name="Universal" set="UBE SE 2.0 Release Body">
    <Group name="UBE"/>
    <SetSlider name="GluteSize p|n" size="big" value="100"/>
  </Preset>
  <Preset name="Male" set="HIMBO Body - SOS">
    <Group name="HIMBO"/>
    <SetSlider name="PecsMass" size="small" value="20"/>
    <SetSlider name="PecsMass" size="big" value="40"/>
  </Preset>
</SliderPresets>)xml";
        std::string error;
        const auto ube = ParseMatches(xml, "Universal", error);
        CHECK(error.empty());
        CHECK(ube.size() == 1);
        CHECK(ube[0].family == Family::kUBE);
        const auto himbo = ParseMatches(xml, "Male", error);
        CHECK(himbo.size() == 1);
        CHECK(himbo[0].family == Family::kHIMBO);
    }

    {
        // Exact OBody 4.4.x UNP behavior: only its ten default sliders invert,
        // and only endpoints actually present in XML invert. Missing remains 0.
        constexpr std::string_view xml = R"xml(
<SliderPresets>
  <Preset name="UNP" set="BHUNP 3BBB Advanced">
    <SetSlider name="Breasts" size="small" value="25"/>
    <SetSlider name="Breasts" size="big" value="100"/>
    <SetSlider name="Arms" size="big" value="40"/>
    <SetSlider name="Waist" size="small" value="25"/>
  </Preset>
</SliderPresets>)xml";
        std::string error;
        const auto matches = ParseMatches(xml, "UNP", error);
        CHECK(matches.size() == 1);
        CHECK(IsUnpSourceSet(matches[0].sourceSet));
        const auto* breasts = FindSlider(matches[0], "Breasts");
        CheckNear(breasts->small, 0.75f);
        CheckNear(breasts->big, 0.0f);
        const auto* arms = FindSlider(matches[0], "Arms");
        CheckNear(arms->small, 0.0f);
        CheckNear(arms->big, 0.6f);
        CheckNear(FindSlider(matches[0], "Waist")->small, 0.25f);
    }

    {
        constexpr std::string_view malformed = R"xml(
<SliderPresets><Preset name="Bad" set="3BA">
  <SetSlider name="Waist" size="sideways" value="nan"/>
</Preset></SliderPresets>)xml";
        std::string error;
        const auto matches = ParseMatches(malformed, "Bad", error);
        CHECK(matches.empty());
        CHECK(!error.empty());
    }

    {
        const auto root = std::filesystem::current_path() /
                          "body-studio-proof-data-tests-tmp";
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        ec.clear();
        CHECK(std::filesystem::create_directory(root, ec));
        {
            std::ofstream(root / "one.xml")
                << "<SliderPresets><Preset name=\"Duplicate\" set=\"3BA\">"
                   "<SetSlider name=\"Waist\" size=\"big\" value=\"50\"/>"
                   "</Preset></SliderPresets>";
            std::ofstream(root / "two.xml")
                << "<SliderPresets><Preset name=\"Duplicate\" set=\"UBE\">"
                   "<SetSlider name=\"Waist\" size=\"big\" value=\"50\"/>"
                   "</Preset></SliderPresets>";
            std::ofstream oversized(root / "oversized.xml", std::ios::binary);
            oversized.seekp(4 * 1024 * 1024);
            oversized.put('\0');
        }
        const auto ambiguous = ResolveInstalled(root, "Duplicate");
        CHECK(!ambiguous.found);
        CHECK(ambiguous.matches == 2);
        CHECK(ambiguous.filesRejected == 1);
        CHECK(!ambiguous.error.empty());

        std::filesystem::remove_all(root, ec);
        CHECK(!ec);
    }

    if (g_failures == 0) {
        std::cout << "BodyStudioProofDataTests: all passed\n";
    }
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
