// Shared dye unlock tests (OS-198). No SKSE, no engine, no filesystem: the
// account-wide file is a JSON document and an add-only merge, and both halves
// are provable without a save.
//
// What is NOT tested here, and cannot be: Load and Save touch the disk. The
// rules that matter are the pure ones below, plus the unreadable latch, which
// is reachable because Save consults it before it reaches the filesystem.
#include "SharedDyeUnlocks.h"

#include <json/json.h>

#include <cstdio>
#include <memory>
#include <set>
#include <string>

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

using namespace OS;

int main() {
    {  // round trip
        const std::set<std::string> ids{ "eso:obsidian-black", "eso:rose-gold",
                                         "custom:my own colour" };
        const auto                  text = SharedDyeUnlocks::EncodeSharedUnlocks(ids);
        std::set<std::string>       back;
        CHECK(SharedDyeUnlocks::DecodeSharedUnlocks(text, back));
        CHECK(back == ids);
    }

    {  // an empty set round trips as an empty set rather than as a refusal
        const auto            text = SharedDyeUnlocks::EncodeSharedUnlocks({});
        std::set<std::string> back{ "stale" };
        CHECK(SharedDyeUnlocks::DecodeSharedUnlocks(text, back));
        CHECK(back.empty());
    }

    {  // ⚠⚠ THE SCHEMA CANNOT CARRY THE LEDGER, and this is the test that says
       // so out loud. Charge and deeds going account-wide is OS-172 rebuilt,
       // so a future edit adding either has to break this block on the way
       // past.
       //
       // ⚠ IT ASSERTS THE MEMBER SET, NOT A SUBSTRING, and the first cut of
       // this test got that wrong: it searched the raw text for "charge" and
       // duly found it in the file's own readme sentence explaining that the
       // charge is NOT stored. A substring search cannot tell a field from
       // prose about the field. The member list can.
        DyeUnlockSet set;
        set.SetCharge(640);
        set.BumpDeed("channelsDyed", 12);
        CHECK(set.Add("eso:rose-gold"));
        const auto text = SharedDyeUnlocks::EncodeSharedUnlocks(set.Ids());

        Json::Value                             root;
        Json::CharReaderBuilder                 rb;
        const std::unique_ptr<Json::CharReader> reader{ rb.newCharReader() };
        std::string                             errs;
        CHECK(reader && reader->parse(text.data(), text.data() + text.size(), &root,
                                      &errs));
        CHECK(root.isObject());
        const auto members = root.getMemberNames();
        CHECK(members.size() == 2);
        CHECK(root.isMember("_readme"));
        CHECK(root.isMember(std::string{ SharedDyeUnlocks::kRootKey }));
        // The values themselves are absent, not merely un-keyed.
        CHECK(text.find("640") == std::string::npos);
        CHECK(text.find("channelsDyed") == std::string::npos);
        CHECK(root[std::string{ SharedDyeUnlocks::kRootKey }].size() == 1);
        CHECK(root[std::string{ SharedDyeUnlocks::kRootKey }][0].asString() ==
              "eso:rose-gold");
    }

    {  // a file with no array at all is a readable EMPTY file, not a refusal:
       // it is what a player who just turned the setting on has
        std::set<std::string> back{ "stale" };
        CHECK(SharedDyeUnlocks::DecodeSharedUnlocks("{}", back));
        CHECK(back.empty());
    }

    {  // garbage is refused, and ⚠ THE OUT SET IS UNTOUCHED, which is the
       // all-or-nothing promise the header makes
        std::set<std::string> keep{ "eso:kept" };
        CHECK(!SharedDyeUnlocks::DecodeSharedUnlocks("not json at all", keep));
        CHECK(keep.size() == 1 && keep.contains("eso:kept"));

        // A root that is not an object would throw inside jsoncpp if the key
        // were reached before the type test.
        CHECK(!SharedDyeUnlocks::DecodeSharedUnlocks("[1,2,3]", keep));
        CHECK(keep.size() == 1);

        // Present and the wrong type is a file saying something this code does
        // not understand, unlike an absent one.
        CHECK(!SharedDyeUnlocks::DecodeSharedUnlocks(R"({"unlocked":"nope"})", keep));
        CHECK(keep.size() == 1);
    }

    {  // ⚠ ONE BAD ELEMENT FAILS THE WHOLE READ and leaves nothing behind.
       // Per-entry tolerance is the dye PACKS' rule, because those are hand
       // authored; this file is machine written, so a bad element means
       // corruption and merging the good half then writing it back is how a
       // partial set overwrites a complete one.
        std::set<std::string> back{ "eso:kept" };
        CHECK(!SharedDyeUnlocks::DecodeSharedUnlocks(
            R"({"unlocked":["eso:good", 7, "eso:also-good"]})", back));
        CHECK(back.size() == 1 && back.contains("eso:kept"));

        CHECK(!SharedDyeUnlocks::DecodeSharedUnlocks(
            R"({"unlocked":["eso:good", ""]})", back));
        CHECK(back.size() == 1);
    }

    {  // the merge is ADD ONLY, and running it twice changes nothing the
       // second time
        DyeUnlockSet set;
        CHECK(set.Add("eso:already-here"));

        const std::set<std::string> file{ "eso:already-here", "eso:from-another-save",
                                          "eso:and-another" };

        const auto first = SharedDyeUnlocks::MergeInto(file, set);
        CHECK(first.gained == 2);   // NOT 3: the one already held is not a gain
        CHECK(first.refused == 0);
        CHECK(set.Size() == 3);
        CHECK(set.Has("eso:from-another-save"));
        CHECK(set.Has("eso:already-here"));

        const auto second = SharedDyeUnlocks::MergeInto(file, set);
        CHECK(second.gained == 0);
        CHECK(second.refused == 0);
        CHECK(set.Size() == 3);
    }

    {  // the merge NEVER removes: an id the character has and the file does
       // not survives, which is what makes the union add-only in both
       // directions rather than a replace dressed up as a merge
        DyeUnlockSet set;
        CHECK(set.Add("eso:only-on-this-character"));
        const auto report = SharedDyeUnlocks::MergeInto({ "eso:only-in-the-file" }, set);
        CHECK(report.gained == 1);
        CHECK(set.Size() == 2);
        CHECK(set.Has("eso:only-on-this-character"));
    }

    {  // ⚠ THE MERGE GOES THROUGH Add, SO Add's INVARIANTS HOLD. An empty id
       // and an over-long one are REFUSED and COUNTED, never inserted: the
       // caps are part of the co-save wire format, so a set carrying an
       // illegal id encodes a record the save cannot read back.
        DyeUnlockSet set;
        const std::string  tooLong(300, 'x');   // kMaxStrLen is 256
        const auto         report =
            SharedDyeUnlocks::MergeInto({ "", tooLong, "eso:fine" }, set);
        CHECK(report.gained == 1);
        CHECK(report.refused == 2);
        CHECK(set.Size() == 1);
        CHECK(set.Has("eso:fine"));
        CHECK(!set.Has(tooLong));
    }

    {  // ⚠ AT THE CAP THE LOSS IS REPORTED RATHER THAN SILENT. A merge that
       // drops shared colours because the set is full looks exactly like a
       // merge with nothing to add, and "my colours did not come across"
       // deserves a number in the log rather than two indistinguishable
       // silences.
        DyeUnlockSet          set;
        std::set<std::string> file;
        for (int i = 0; i < 2100; ++i) {   // kMaxUnlocks is 2048
            file.insert("eso:filler-" + std::to_string(i));
        }
        const auto report = SharedDyeUnlocks::MergeInto(file, set);
        CHECK(report.gained == 2048);
        CHECK(report.refused == 2100 - 2048);
        CHECK(set.Size() == 2048);
    }

    {  // the unreadable latch: a session that could not READ the file must not
       // WRITE it, because Save replaces it whole. Reachable without a disk
       // because Save consults the latch first.
        SharedDyeUnlocks::ResetForTests();
        CHECK(!SharedDyeUnlocks::Unreadable());
    }

    if (g_failures == 0) {
        std::printf("SharedDyeUnlocksTests: all passed\n");
        return 0;
    }
    std::printf("SharedDyeUnlocksTests: %d failure(s)\n", g_failures);
    return 1;
}
