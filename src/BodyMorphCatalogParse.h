#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Reading the installed body's own slider vocabulary out of BodySlide's files.
//
// ⚠ THE BODY AUTHOR SHIPS THE VOCABULARY AND THAT IS WHY THIS FEATURE IS
// POSSIBLE AT ALL. A hand-written table of morph names cannot work: the names
// are per body and per author, and the reference load order's UBE build alone
// exposes 216 of them with names like "Upper_TorsoSize n|p" and "Thicc
// forearms". What every body DOES ship is a SliderCategories file grouping
// those names under headings a person wrote, with a display name each. Reading
// that gets a SAM-shaped page on any body, including ones that do not exist
// yet, with nothing curated by us.
//
// ⚠ TOLERANT BY NECESSITY, NOT BY PREFERENCE. These files are hand-maintained
// and some shipped ones are not well-formed XML: CBBE's own CBBE.xml contains
// `name="Hips"displayname="Size"` with no space between the attributes, which a
// conforming parser rejects, and rejecting it drops every CBBE category at
// once. BodySlideCatalog.cpp runs those files through tinyxml2 and counts the
// failure in rejectedFiles, which is the same bug sitting quietly one module
// over. So this scans tags rather than parsing a document, and a malformed
// attribute costs that attribute instead of the file.
//
// Pure string work on purpose: no engine type, no filesystem, so the awkward
// half is covered by tests/test_bodymorphcatalog.cpp and the disk walk in
// BodyMorphCatalog.cpp stays thin enough to read.
namespace OS::BodyMorphCatalogParse {

    // A name longer than this is a corrupt file rather than a slider.
    inline constexpr std::size_t kMaxNameBytes = 256;
    // Enough for the largest real body several times over. UBE's category files
    // describe about 380 sliders across 27 categories.
    inline constexpr std::size_t kMaxSliders    = 4096;
    inline constexpr std::size_t kMaxCategories = 512;

    struct SliderEntry {
        std::string name;     // what SetMorph wants
        std::string display;  // what the page shows; falls back to name

        friend bool operator==(const SliderEntry&, const SliderEntry&) = default;
    };

    struct Category {
        std::string              name;
        bool                     defaultHidden{ false };
        std::vector<SliderEntry> sliders;

        friend bool operator==(const Category&, const Category&) = default;
    };

    namespace detail {

        // XML's five predefined entities. UBE's slider names really do carry
        // them: "NeckSize__HEAD&gt;" is a slider, and the name SetMorph wants is
        // the decoded one.
        [[nodiscard]] inline std::string Decode(std::string_view a_text) {
            std::string out;
            out.reserve(a_text.size());
            for (std::size_t i = 0; i < a_text.size();) {
                if (a_text[i] != '&') {
                    out.push_back(a_text[i++]);
                    continue;
                }
                const auto semi = a_text.find(';', i + 1);
                if (semi == std::string_view::npos || semi - i > 6) {
                    out.push_back(a_text[i++]);
                    continue;
                }
                const auto entity = a_text.substr(i + 1, semi - i - 1);
                if (entity == "amp") {
                    out.push_back('&');
                } else if (entity == "lt") {
                    out.push_back('<');
                } else if (entity == "gt") {
                    out.push_back('>');
                } else if (entity == "quot") {
                    out.push_back('"');
                } else if (entity == "apos") {
                    out.push_back('\'');
                } else {
                    out.push_back(a_text[i++]);
                    continue;
                }
                i = semi + 1;
            }
            return out;
        }

        // One tag's worth of the scan.
        struct Tag {
            std::string_view name;     // "Category", "/Category", "Slider"
            std::string_view attrs;    // everything between the name and > or />
            std::size_t      end{ 0 }; // index just past the >
            bool             selfClosing{ false };
        };

        // The next tag at or after a_from. Quotes are respected so a > inside an
        // attribute value cannot end the tag early, which matters because slider
        // names contain them.
        [[nodiscard]] inline bool NextTag(std::string_view a_text, std::size_t a_from,
                                          Tag& a_out) {
            while (true) {
                const auto open = a_text.find('<', a_from);
                if (open == std::string_view::npos) {
                    return false;
                }
                // Skip comments, declarations and doctypes wholesale.
                if (a_text.compare(open, 4, "<!--") == 0) {
                    const auto close = a_text.find("-->", open + 4);
                    if (close == std::string_view::npos) return false;
                    a_from = close + 3;
                    continue;
                }
                if (open + 1 < a_text.size() &&
                    (a_text[open + 1] == '?' || a_text[open + 1] == '!')) {
                    const auto close = a_text.find('>', open + 1);
                    if (close == std::string_view::npos) return false;
                    a_from = close + 1;
                    continue;
                }
                std::size_t i     = open + 1;
                bool        quote = false;
                char        qch   = '\0';
                while (i < a_text.size()) {
                    const char c = a_text[i];
                    if (quote) {
                        if (c == qch) quote = false;
                    } else if (c == '"' || c == '\'') {
                        quote = true;
                        qch   = c;
                    } else if (c == '>') {
                        break;
                    }
                    ++i;
                }
                if (i >= a_text.size()) {
                    return false;
                }
                std::size_t nameEnd = open + 1;
                while (nameEnd < i && !std::isspace(static_cast<unsigned char>(a_text[nameEnd])) &&
                       a_text[nameEnd] != '/' && a_text[nameEnd] != '>') {
                    ++nameEnd;
                }
                a_out.name        = a_text.substr(open + 1, nameEnd - (open + 1));
                a_out.selfClosing = i > open && a_text[i - 1] == '/';
                const auto attrEnd = a_out.selfClosing ? i - 1 : i;
                a_out.attrs =
                    nameEnd < attrEnd ? a_text.substr(nameEnd, attrEnd - nameEnd)
                                      : std::string_view{};
                a_out.end = i + 1;
                return true;
            }
        }

        // ⚠ FINDS AN ATTRIBUTE WITHOUT REQUIRING THE SPACE BEFORE IT. The whole
        // reason this file scans instead of parses: a shipped CBBE.xml writes
        // `name="Hips"displayname="Size"`, so the character before the attribute
        // name may be a quote rather than whitespace.
        [[nodiscard]] inline std::string Attr(std::string_view a_attrs,
                                              std::string_view a_key) {
            for (std::size_t i = 0; i + a_key.size() < a_attrs.size();) {
                const auto at = a_attrs.find(a_key, i);
                if (at == std::string_view::npos) {
                    return {};
                }
                // Must start a token: preceded by space or a closing quote, and
                // followed by '=' (allowing spaces around it).
                const bool boundedLeft =
                    at == 0 || std::isspace(static_cast<unsigned char>(a_attrs[at - 1])) ||
                    a_attrs[at - 1] == '"' || a_attrs[at - 1] == '\'';
                std::size_t j = at + a_key.size();
                while (j < a_attrs.size() &&
                       std::isspace(static_cast<unsigned char>(a_attrs[j]))) {
                    ++j;
                }
                if (!boundedLeft || j >= a_attrs.size() || a_attrs[j] != '=') {
                    i = at + 1;
                    continue;
                }
                ++j;
                while (j < a_attrs.size() &&
                       std::isspace(static_cast<unsigned char>(a_attrs[j]))) {
                    ++j;
                }
                if (j >= a_attrs.size() || (a_attrs[j] != '"' && a_attrs[j] != '\'')) {
                    i = at + 1;
                    continue;
                }
                const char qch   = a_attrs[j];
                const auto start = ++j;
                const auto close = a_attrs.find(qch, start);
                if (close == std::string_view::npos) {
                    return {};
                }
                const auto raw = a_attrs.substr(start, close - start);
                return raw.size() > kMaxNameBytes ? std::string{} : Decode(raw);
            }
            return {};
        }

        [[nodiscard]] inline bool AttrIsTrue(std::string_view a_attrs,
                                             std::string_view a_key) {
            auto v = Attr(a_attrs, a_key);
            std::ranges::transform(v, v.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return v == "true" || v == "1";
        }

    }  // namespace detail

    // Categories out of one SliderCategories file.
    //
    // A Slider outside any Category is dropped rather than collected into an
    // "Other" bucket: BodySlide itself would not show it, and inventing a home
    // for it here would put sliders on the page that the body's own tool hides.
    [[nodiscard]] inline std::vector<Category> ParseCategories(std::string_view a_text) {
        std::vector<Category> out;
        detail::Tag           tag;
        std::size_t           at   = 0;
        bool                  open = false;
        while (detail::NextTag(a_text, at, tag)) {
            at = tag.end;
            if (tag.name == "Category") {
                if (out.size() >= kMaxCategories) {
                    break;
                }
                Category c;
                c.name          = detail::Attr(tag.attrs, "name");
                c.defaultHidden = detail::AttrIsTrue(tag.attrs, "defaultHidden");
                if (c.name.empty()) {
                    open = false;
                    continue;
                }
                out.push_back(std::move(c));
                open = !tag.selfClosing;
            } else if (tag.name == "/Category") {
                open = false;
            } else if (tag.name == "Slider" && open && !out.empty()) {
                auto name = detail::Attr(tag.attrs, "name");
                if (name.empty()) {
                    continue;
                }
                auto display = detail::Attr(tag.attrs, "displayname");
                auto& sliders = out.back().sliders;
                if (sliders.size() >= kMaxSliders) {
                    continue;
                }
                sliders.push_back(
                    SliderEntry{ name, display.empty() ? name : std::move(display) });
            }
        }
        // A category with nothing in it is a heading with no controls.
        std::erase_if(out, [](const Category& a_c) { return a_c.sliders.empty(); });
        return out;
    }

    // The morph names one slider set actually carries.
    //
    // ⚠ hidden AND uv SLIDERS ARE DROPPED. A hidden slider is one the body
    // author does not want touched directly, and a uv slider moves texture
    // coordinates rather than vertices, so neither belongs on a page of body
    // shape. BodySlide draws neither.
    //
    // a_setName empty means take every set in the file.
    [[nodiscard]] inline std::set<std::string> ParseSliderSetNames(
        std::string_view a_text, std::string_view a_setName) {
        std::set<std::string> out;
        detail::Tag           tag;
        std::size_t           at     = 0;
        bool                  active = a_setName.empty();
        while (detail::NextTag(a_text, at, tag)) {
            at = tag.end;
            if (tag.name == "SliderSet") {
                active = a_setName.empty() ||
                         detail::Attr(tag.attrs, "name") == a_setName;
            } else if (tag.name == "/SliderSet") {
                active = a_setName.empty();
            } else if (tag.name == "Slider" && active) {
                if (detail::AttrIsTrue(tag.attrs, "hidden") ||
                    detail::AttrIsTrue(tag.attrs, "uv")) {
                    continue;
                }
                auto name = detail::Attr(tag.attrs, "name");
                if (!name.empty() && out.size() < kMaxSliders) {
                    out.insert(std::move(name));
                }
            }
        }
        return out;
    }

    // Which slider set a named BodySlide preset was built against. Empty when
    // the file does not carry that preset.
    [[nodiscard]] inline std::string ParsePresetSet(std::string_view a_text,
                                                    std::string_view a_presetName) {
        detail::Tag tag;
        std::size_t at = 0;
        while (detail::NextTag(a_text, at, tag)) {
            at = tag.end;
            if (tag.name != "Preset") {
                continue;
            }
            if (detail::Attr(tag.attrs, "name") == a_presetName) {
                return detail::Attr(tag.attrs, "set");
            }
        }
        return {};
    }

    // ---- BodySlide preset XML, which is also SAM's save format -------------
    //
    // ⚠ SCREEN ARCHER MENU SAVES AND LOADS BODY MORPHS AS BODYSLIDE PRESET XML.
    // Its sam/menu/BodyMorphs.yaml points SaveBodyMorphs and LoadBodyMorphs at
    // Data\CalienteTools\BodySlide\SliderPresets with ext .xml. So writing that
    // format is not an integration with SAM, it IS the integration: a file in
    // that shape is readable by SAM, by BodySlide itself and by OBody, and no
    // bespoke format buys anything over it.
    //
    // ⚠ THE FILE IS IN PERCENT AND WE WORK IN FRACTIONS. BodySlide writes
    // value="70" where RaceMenu's SetMorph wants 0.7, and SAM's own slider runs
    // -100..200 for the same reason. Convert at this boundary and nowhere else.
    inline constexpr float kPercentPerUnit = 100.0f;

    // ⚠ big WINS AND small IS THE FALLBACK. A BodySlide preset carries a value
    // for each weight endpoint, and an overlay has one number. Presets Fitting
    // Room writes set both to the same thing, which is what SAM's own Save
    // Weight "Both" does, so this only has to choose for foreign files; taking
    // the big end matches what a character at weight 100 is wearing and is a
    // defined answer where averaging would invent a third value.
    [[nodiscard]] inline std::vector<std::pair<std::string, float>> ParsePresetSliders(
        std::string_view a_text, std::string_view a_presetName) {
        std::vector<std::pair<std::string, float>> out;
        std::vector<std::string>                   order;
        std::set<std::string>                      seenBig;
        detail::Tag                                tag;
        std::size_t                                at     = 0;
        bool                                       active = false;
        while (detail::NextTag(a_text, at, tag)) {
            at = tag.end;
            if (tag.name == "Preset") {
                active = detail::Attr(tag.attrs, "name") == a_presetName;
                continue;
            }
            if (tag.name == "/Preset") {
                active = false;
                continue;
            }
            if (tag.name != "SetSlider" || !active) {
                continue;
            }
            auto name = detail::Attr(tag.attrs, "name");
            if (name.empty() || out.size() >= kMaxSliders) {
                continue;
            }
            const auto size = detail::Attr(tag.attrs, "size");
            const auto raw  = detail::Attr(tag.attrs, "value");
            if (raw.empty()) {
                continue;
            }
            float value = 0.0f;
            try {
                value = std::stof(raw) / kPercentPerUnit;
            } catch (...) {
                continue;  // a value that is not a number is a corrupt row
            }
            const bool isBig = size == "big";
            auto       found = std::ranges::find_if(
                out, [&](const auto& a_p) { return a_p.first == name; });
            if (found == out.end()) {
                out.emplace_back(name, value);
                if (isBig) {
                    seenBig.insert(name);
                }
            } else if (isBig && !seenBig.contains(name)) {
                found->second = value;
                seenBig.insert(name);
            }
        }
        return out;
    }

    // The document Fitting Room writes. Both endpoints get the same value, so a
    // shape looks the same at every weight, which is what an overlay means.
    //
    // ⚠ ZERO VALUES ARE OMITTED. A preset is a set of edits, and writing every
    // untouched slider as 0 would make a two-slider shape a 400-line file that
    // also stamps a zero over anything already on the character when SAM loads
    // it.
    [[nodiscard]] inline std::string BuildPresetXml(
        std::string_view a_presetName, std::string_view a_setName,
        const std::vector<std::pair<std::string, float>>& a_values) {
        const auto escape = [](std::string_view a_in) {
            std::string out;
            out.reserve(a_in.size());
            for (const char c : a_in) {
                switch (c) {
                    case '&': out += "&amp;"; break;
                    case '<': out += "&lt;"; break;
                    case '>': out += "&gt;"; break;
                    case '"': out += "&quot;"; break;
                    case '\'': out += "&apos;"; break;
                    default: out.push_back(c); break;
                }
            }
            return out;
        };
        const auto number = [](float a_value) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.6g", a_value * kPercentPerUnit);
            return std::string(buf);
        };

        std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<SliderPresets>\n";
        out += "    <Preset name=\"" + escape(a_presetName) + "\" set=\"" +
               escape(a_setName) + "\">\n";
        for (const auto& [name, value] : a_values) {
            if (value == 0.0f || name.empty()) {
                continue;
            }
            const auto escaped = escape(name);
            const auto text    = number(value);
            out += "        <SetSlider name=\"" + escaped + "\" size=\"small\" value=\"" +
                   text + "\"/>\n";
            out += "        <SetSlider name=\"" + escaped + "\" size=\"big\" value=\"" +
                   text + "\"/>\n";
        }
        out += "    </Preset>\n</SliderPresets>\n";
        return out;
    }

    // A file name that cannot escape its folder or upset Windows. Presets are
    // named by the user and the name reaches the filesystem.
    [[nodiscard]] inline std::string SafeFileStem(std::string_view a_name) {
        std::string out;
        out.reserve(a_name.size());
        for (const char c : a_name) {
            const unsigned char u = static_cast<unsigned char>(c);
            const bool bad = u < 0x20 || c == '\\' || c == '/' || c == ':' ||
                             c == '*' || c == '?' || c == '"' || c == '<' ||
                             c == '>' || c == '|';
            out.push_back(bad ? '_' : c);
        }
        while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
            out.pop_back();  // Windows refuses both as a trailing character
        }
        while (!out.empty() && out.front() == ' ') {
            out.erase(out.begin());
        }
        if (out.size() > 96) {
            out.resize(96);
        }
        return out;
    }

    // Drop every slider the built body does not have, and every category left
    // empty by that.
    //
    // ⚠ THIS IS THE WHOLE REASON THE SLIDER SET IS READ AT ALL. Several bodies'
    // category files are installed side by side (the reference load order has
    // CBBE, 3BA, HIMBO, TNG and UBE), and a morph name the body lacks is stored
    // by RaceMenu without complaint and moves nothing. Unfiltered, the page
    // would offer hundreds of controls that silently do nothing, which is worse
    // than offering none.
    //
    // An EMPTY available set means "we could not work out which body is built",
    // and everything is kept. A page listing too much is recoverable; a page
    // listing nothing looks broken.
    [[nodiscard]] inline std::vector<Category> KeepAvailable(
        const std::vector<Category>& a_categories,
        const std::set<std::string>& a_available) {
        if (a_available.empty()) {
            return a_categories;
        }
        std::vector<Category> out;
        out.reserve(a_categories.size());
        for (const auto& c : a_categories) {
            Category kept;
            kept.name          = c.name;
            kept.defaultHidden = c.defaultHidden;
            for (const auto& s : c.sliders) {
                if (a_available.contains(s.name)) {
                    kept.sliders.push_back(s);
                }
            }
            if (!kept.sliders.empty()) {
                out.push_back(std::move(kept));
            }
        }
        return out;
    }

    // Merge the categories of several files, keeping first-seen order and
    // dropping a slider already placed by an earlier file.
    //
    // ⚠ FIRST FILE WINS A SLIDER, not last. Two bodies installed together will
    // both claim common names like "Breasts", and the alternative to a rule is
    // the same control appearing under two headings.
    [[nodiscard]] inline std::vector<Category> Merge(
        const std::vector<std::vector<Category>>& a_files) {
        std::vector<Category> out;
        std::set<std::string> placed;
        for (const auto& file : a_files) {
            for (const auto& c : file) {
                auto at = std::ranges::find_if(
                    out, [&](const Category& a_o) { return a_o.name == c.name; });
                if (at == out.end()) {
                    if (out.size() >= kMaxCategories) {
                        continue;
                    }
                    out.push_back(Category{ c.name, c.defaultHidden, {} });
                    at = out.end() - 1;
                }
                for (const auto& s : c.sliders) {
                    if (placed.insert(s.name).second) {
                        at->sliders.push_back(s);
                    }
                }
            }
        }
        std::erase_if(out, [](const Category& a_c) { return a_c.sliders.empty(); });
        return out;
    }

    // Case-insensitive substring match over what the user can actually read.
    // The raw morph name is searched too, because a body's display names are
    // sometimes bare ("Size" appears under six headings) and the underlying name
    // is the only thing that tells two of them apart.
    [[nodiscard]] inline bool Matches(const SliderEntry& a_slider,
                                      std::string_view    a_search) {
        if (a_search.empty()) {
            return true;
        }
        const auto lower = [](std::string_view a_in) {
            std::string s(a_in);
            std::ranges::transform(s, s.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return s;
        };
        const auto needle = lower(a_search);
        return lower(a_slider.display).find(needle) != std::string::npos ||
               lower(a_slider.name).find(needle) != std::string::npos;
    }

    [[nodiscard]] inline std::size_t CountSliders(const std::vector<Category>& a_cats) {
        std::size_t n = 0;
        for (const auto& c : a_cats) {
            n += c.sliders.size();
        }
        return n;
    }

}  // namespace OS::BodyMorphCatalogParse
