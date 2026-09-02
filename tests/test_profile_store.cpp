// Profile store tests: filesystem CRUD against a temp directory, the same
// harness shape as the body preset store's. Profile content reuses the
// real-capture values test_profilecodec.cpp documents (the purp colour, the
// Sindra OBody preset, the UBE nail overlay).
#include "ProfileStore.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
    int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL " << __LINE__ << ": " #x "\n"; ++failures; } } while (false)

    OS::ProfileCodec::Profile Forge(const std::string& a_name) {
        OS::ProfileCodec::Profile p;
        p.name = a_name;
        p.face = OS::ProfileCodec::FaceBlock{
            "FR_" + a_name, OS::ProfileCodec::FaceSource::kCaptured, 0
        };
        p.body = OS::ProfileCodec::BodyBlock{ "Sindra", std::nullopt };
        p.skin = OS::ProfileCodec::SkinBlock{ "UBE" };
        return p;
    }

    std::size_t JsonFiles(const std::filesystem::path& a_root) {
        std::size_t files = 0;
        for (const auto& entry : std::filesystem::directory_iterator(a_root)) {
            if (entry.path().extension() == ".json") ++files;
        }
        return files;
    }
}

int main() {
    using namespace OS;
    const auto root = std::filesystem::current_path() / "profile-store-tests-tmp";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    ProfileStore store(root);
    store.Load();
    CHECK(store.Snapshot().empty());
    CHECK(store.RejectedCount() == 0);

    std::string error;
    CHECK(store.Save(Forge("Aria"), error));
    CHECK(store.Snapshot().size() == 1);
    CHECK(store.Find("Aria").has_value());
    CHECK(store.Find("aria").has_value());  // names are case-insensitive
    CHECK(store.Find("Aria")->rewritable);
    CHECK(store.Find("Aria")->file.filename() == "Aria.json");
    CHECK(!store.NameAvailable("ARIA"));
    CHECK(store.NameAvailable("ARIA", "Aria"));  // her own name is not taken

    {  // saving the same name again replaces the file rather than uniquify
        auto again = Forge("Aria");
        again.skin = ProfileCodec::SkinBlock{ "UBE-2" };
        CHECK(store.Save(again, error));
        CHECK(JsonFiles(root) == 1);
        CHECK(store.Find("Aria")->profile.skin->pack == "UBE-2");
    }

    {  // the uniquifier: two names that sanitize to one filename get two files
        CHECK(store.Save(Forge("Aria?"), error));   // sanitizes to "Aria_"
        CHECK(store.Save(Forge("Aria_"), error));   // collides on disk
        CHECK(store.Snapshot().size() == 3);
        CHECK(JsonFiles(root) == 3);
        const auto a = store.Find("Aria?");
        const auto b = store.Find("Aria_");
        CHECK(a && b && a->file != b->file);
        CHECK(a->file.filename() == "Aria_.json");
        CHECK(b->file.filename() == "Aria_-2.json");
    }

    {  // rename: new file, old file gone, taken names refused
        CHECK(store.Rename("Aria?", "Callisto", error));
        CHECK(!store.Find("Aria?"));
        CHECK(store.Find("Callisto"));
        CHECK(store.Find("Callisto")->file.filename() == "Callisto.json");
        CHECK(JsonFiles(root) == 3);
        CHECK(!store.Rename("Callisto", "Aria", error));
        CHECK(error.find("already uses") != std::string::npos);
        CHECK(!store.Rename("Nobody", "Anybody", error));
    }

    {  // delete hands back the captured jslot; a referenced one stays private
        std::string jslot;
        CHECK(store.Delete("Callisto", error, &jslot));
        CHECK(jslot == "FR_Aria?");  // captured under the original name
        CHECK(!store.Find("Callisto"));

        auto referenced = Forge("Redguard");
        referenced.face->source = ProfileCodec::FaceSource::kReferenced;
        referenced.face->jslot  = "Redguard";
        CHECK(store.Save(referenced, error));
        jslot = "sentinel";
        CHECK(store.Delete("Redguard", error, &jslot));
        CHECK(jslot.empty());
    }

    {  // a file whose known blocks all failed loads flagged and refuses writes
        std::filesystem::create_directories(root, ec);
        std::ofstream out(root / "Broken.json", std::ios::trunc);
        out << R"({"version":1,"name":"Broken","outfit":"junk","weight":null})";
        out.close();
        store.Load();
        const auto broken = store.Find("Broken");
        CHECK(broken.has_value());
        CHECK(!broken->rewritable);
        CHECK(broken->dropped.size() == 2);
        CHECK(!store.Save(Forge("Broken"), error));
        CHECK(error.find("Broken.json") != std::string::npos);
        CHECK(!store.Rename("Broken", "Fixed", error));
        std::string jslot;
        CHECK(store.Delete("Broken", error, &jslot));  // deleting stays legal
    }

    {  // junk files and duplicate names count as rejected, first name wins
        std::ofstream(root / "AAA-dup.json", std::ios::trunc)
            << R"({"version":1,"name":"Aria_"})";
        std::ofstream(root / "not-json.json", std::ios::trunc) << "{nope";
        store.Load();
        CHECK(store.RejectedCount() == 2);
        // Files load in path order, so AAA-dup.json claims the name first and
        // the original Aria_.json is the rejected duplicate this round.
        const auto kept = store.Find("Aria_");
        CHECK(kept && kept->file.filename() == "AAA-dup.json");
    }

    std::filesystem::remove_all(root, ec);
    CHECK(!ec);
    if (!failures) std::cout << "ProfileStoreTests: all passed\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
