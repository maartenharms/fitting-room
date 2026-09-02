#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Saved body shapes: a named set of morph values you can put on any character.
//
// ⚠ THE FILE FORMAT IS BODYSLIDE'S PRESET XML, WHICH IS ALSO SAM'S SAVE FORMAT.
// Screen Archer Menu's sam/menu/BodyMorphs.yaml points SaveBodyMorphs and
// LoadBodyMorphs at Data\CalienteTools\BodySlide\SliderPresets with ext .xml,
// so a file in that shape is already readable by SAM, by BodySlide and by
// OBody. Writing anything of our own would have been a format only we could
// read, for no gain.
//
// ⚠ TWO LOCATIONS, AND THE SPLIT IS DELIBERATE. Shapes live in Fitting Room's
// own folder, and Export writes a COPY into BodySlide's SliderPresets. Saving
// straight into SliderPresets would put every experiment into OBody's body
// preset list and into BodySlide's own, which is somebody else's surface to
// fill without asking. Export is the moment the user says they want it there.
namespace OS::BodyMorphPresets {

    struct Shape {
        std::string id;    // the file stem, which is what makes it unique on disk
        std::string name;  // the Preset name inside the file
        std::string set;   // the slider set it was built against
        std::vector<std::pair<std::string, float>> values;  // fractions, not percent
        bool        exported{ false };  // a copy exists under SliderPresets
    };

    struct Library {
        std::vector<Shape> shapes;
        std::string        diagnostic;
    };

    // Re-read Fitting Room's shape folder. Cheap: a handful of small files.
    void Reload();

    [[nodiscard]] std::shared_ptr<const Library> Get();

    // Write a shape, replacing one of the same name. Returns false and logs
    // when the name is unusable or the write fails.
    bool Save(const std::string& a_name, const std::string& a_set,
              const std::vector<std::pair<std::string, float>>& a_values);

    bool Delete(const std::string& a_id);

    // Copy a shape into Data\CalienteTools\BodySlide\SliderPresets\Fitting Room
    // so SAM, BodySlide and OBody can see it. Toggles off by deleting that copy.
    bool SetExported(const std::string& a_id, bool a_exported);

    [[nodiscard]] const Shape* Find(const Library& a_library, const std::string& a_id);

}  // namespace OS::BodyMorphPresets
