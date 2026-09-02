// Dye scheme tests. No SKSE, no engine - a scheme is a name and a list of
// colours, and the store is JSON on disk.
#include "DyeSchemes.h"

#include <json/json.h>

#include <cstdio>
#include <string>

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

int main() {
    using namespace OS;

    {  // a scheme round trips through JSON
        DyeScheme s;
        s.name    = "Nightingale";
        s.colours = { DyeChannel{ true, 10, 20, 30 }, DyeChannel{ true, 40, 50, 60 } };

        const auto json = DyeSchemes::ToJson(s);
        CHECK(json["name"].asString() == "Nightingale");
        CHECK(json["colours"].size() == 2);
        CHECK(json["colours"][0].asString() == "0A141E");  // 10,20,30

        DyeScheme back;
        CHECK(DyeSchemes::FromJson(json, back));
        CHECK(back.name == "Nightingale");
        CHECK(back.colours.size() == 2);
        CHECK(back.colours[1] == (DyeChannel{ true, 40, 50, 60 }));
    }

    {  // untrusted input degrades rather than half-loading
        Json::Value bad;
        bad["name"] = "Broken";
        bad["colours"].append("nonsense");
        bad["colours"].append("0A141E");
        DyeScheme out;
        CHECK(DyeSchemes::FromJson(bad, out));
        CHECK(out.colours.size() == 1);  // the malformed entry is dropped, not guessed

        Json::Value nameless;
        nameless["colours"].append("0A141E");
        DyeScheme unnamed;
        CHECK(!DyeSchemes::FromJson(nameless, unnamed));  // a scheme needs a name
    }

    {  // a name becomes a filename, and cannot escape the schemes folder
        CHECK(DyeSchemes::FileNameFor("Nightingale") == "Nightingale.json");
        // Separators and traversal are the whole risk: the name is free text
        // typed by the user and it is about to be pasted into a path.
        CHECK(DyeSchemes::FileNameFor("../../evil") != "../../evil.json");
        CHECK(DyeSchemes::FileNameFor("../../evil").find('/') == std::string::npos);
        CHECK(DyeSchemes::FileNameFor("a\\b").find('\\') == std::string::npos);
        CHECK(DyeSchemes::FileNameFor("a:b").find(':') == std::string::npos);
        // A name with nothing usable left still has to produce SOME file.
        CHECK(!DyeSchemes::FileNameFor("///").empty());
        CHECK(DyeSchemes::FileNameFor("///") != ".json");
    }

    {  // an unset colour is still a slot in the order, so it round trips
        DyeScheme s;
        s.name    = "Gap";
        s.colours = { DyeChannel{ true, 1, 2, 3 }, DyeChannel{}, DyeChannel{ true, 7, 8, 9 } };
        DyeScheme back;
        CHECK(DyeSchemes::FromJson(DyeSchemes::ToJson(s), back));
        CHECK(back.colours.size() == 3);
        CHECK(!back.colours[1].set);
        CHECK(back.colours[2] == (DyeChannel{ true, 7, 8, 9 }));
    }

    if (g_failures == 0) {
        std::printf("DyeSchemesTests: all passed\n");
        return 0;
    }
    std::printf("DyeSchemesTests: %d failure(s)\n", g_failures);
    return 1;
}
