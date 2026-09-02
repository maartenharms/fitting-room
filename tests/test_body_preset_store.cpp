#include "BodyPresetStore.h"

#include <tinyxml2.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
    int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL " << __LINE__ << ": " #x "\n"; ++failures; } } while (false)
}

int main() {
    using namespace OS;
    const auto root = std::filesystem::current_path() / "body-preset-store-tests-tmp";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    BodyPresetStore store(root);
    store.Load();
    CHECK(store.Snapshot().empty());

    BodyPreset preset;
    preset.name = "My Body";
    preset.sex = BodySex::kFemale;
    preset.family = BodyFamily::k3BA;
    preset.sourceSet = "CBBE 3BBB Body Amazing";
    preset.sourcePreset = "Installed";
    preset.groups = { "3BA" };
    preset.sliders = {
        { "Waist", "Waist Width", "Torso", 20.0f, 80.0f },
        { "Zero", "Zero", "Other", 0.0f, 0.0f },
    };
    std::string error;
    CHECK(store.Save(preset, error));
    CHECK(!preset.id.empty());
    CHECK(store.Snapshot().size() == 1);
    CHECK(store.Snapshot()[0] == preset);
    CHECK(store.Find(preset.id).has_value());
    CHECK(store.Find(preset.id)->name == "My Body");
    CHECK(!store.Find("missing-id").has_value());

    preset.name = "Renamed Body";
    CHECK(store.Save(preset, error));
    std::size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (entry.path().extension() == ".json") ++files;
    }
    CHECK(files == 1);  // rename removed the previous stable-id filename

    BodyPreset duplicate = preset;
    duplicate.id.clear();
    CHECK(!store.Save(duplicate, error));

    const auto exportPath = root / "export.xml";
    CHECK(store.ExportBodySlideXml(exportPath, error));
    tinyxml2::XMLDocument doc;
    CHECK(doc.LoadFile(exportPath.string().c_str()) == tinyxml2::XML_SUCCESS);
    auto* node = doc.FirstChildElement("SliderPresets")->FirstChildElement("Preset");
    CHECK(node != nullptr);
    CHECK(std::string(node->Attribute("name")) == "Renamed Body");
    CHECK(node->FirstChildElement("SetSlider") != nullptr);

    CHECK(store.Delete(preset.id, error));
    CHECK(store.Snapshot().empty());
    CHECK(!store.Find(preset.id).has_value());

    std::filesystem::remove_all(root, ec);
    CHECK(!ec);
    if (!failures) std::cout << "BodyPresetStoreTests: all passed\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
