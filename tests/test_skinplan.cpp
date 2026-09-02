#include "../src/PersistenceCodec.h"
#include "../src/SkinCardScene.h"
#include "../src/SkinImport.h"
#include "../src/SkinPlan.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#define CHECK(x)                                                              \
    do {                                                                      \
        if (!(x)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #x);          \
            std::exit(1);                                                     \
        }                                                                     \
    } while (0)

int main() {
    using namespace OS::SkinPlan;

    {  // A file name is the last segment, lower-cased, whichever slash.
        CHECK(FileName("data\\textures\\actors\\character\\female\\FemaleBody_1.dds") ==
              "femalebody_1.dds");
        CHECK(FileName("Textures/!UBE/Body/femalebody_1_d.dds") == "femalebody_1_d.dds");
        CHECK(FileName("femalehands_1_msn.dds") == "femalehands_1_msn.dds");
        CHECK(FileName("").empty());
    }
    {  // The pack is the first folder under the skins root, and only that.
        CHECK(PackIdOf("FittingRoom\\skins\\Fair Skin\\femalebody_1.dds") == "Fair Skin");
        CHECK(PackIdOf("fittingroom\\SKINS\\Bijin\\female\\femalebody_1_msn.dds") == "Bijin");
        // Loose at the root: no pack.
        CHECK(PackIdOf("FittingRoom\\skins\\femalebody_1.dds").empty());
        // Somewhere else entirely: no pack.
        CHECK(PackIdOf("actors\\character\\female\\femalebody_1.dds").empty());
        CHECK(PackIdOf("").empty());
    }
    {  // Folding scanned paths into packs: by name, first wins, non dds dropped.
        std::vector<Pack> packs;
        CHECK(AddFile(packs, "FittingRoom\\skins\\Fair\\femalebody_1.dds") == "Fair");
        CHECK(AddFile(packs, "FittingRoom\\skins\\Fair\\sub\\FemaleBody_1_msn.dds") == "Fair");
        CHECK(AddFile(packs, "FittingRoom\\skins\\Fair\\readme.txt").empty());
        CHECK(AddFile(packs, "FittingRoom\\skins\\Bijin\\femalebody_1.dds") == "Bijin");
        // A second femalebody_1 in the same pack keeps the first.
        CHECK(AddFile(packs, "FittingRoom\\skins\\Fair\\other\\femalebody_1.dds") == "Fair");
        CHECK(packs.size() == 2);
        CHECK(packs[0].id == "Fair");
        CHECK(packs[0].Count() == 2);
        CHECK(packs[0].files.at("femalebody_1.dds") == "FittingRoom\\skins\\Fair\\femalebody_1.dds");
        CHECK(packs[0].files.at("femalebody_1_msn.dds") ==
              "FittingRoom\\skins\\Fair\\sub\\FemaleBody_1_msn.dds");
        CHECK(packs[1].id == "Bijin");
        CHECK(packs[1].Count() == 1);
    }
    {  // ⚠⚠ THE MATCH IS BY FILE NAME AND NOTHING ELSE. A 3BA body slot reading
        // the vanilla path, a UBE slot reading UBE's own name, and a slot that
        // already wears another pack all resolve by their last segment.
        Pack pack{ "Fair",
                   { { "femalebody_1.dds", "FittingRoom\\skins\\Fair\\femalebody_1.dds" },
                     { "femalebody_1_msn.dds", "FittingRoom\\skins\\Fair\\femalebody_1_msn.dds" },
                     { "femalebody_1_d.dds", "FittingRoom\\skins\\Fair\\ube\\femalebody_1_d.dds" } } };
        CHECK(Match(pack, "data\\textures\\actors\\character\\female\\femalebody_1.dds") ==
              "FittingRoom\\skins\\Fair\\femalebody_1.dds");
        CHECK(Match(pack, "Textures\\!UBE\\Body\\femalebody_1_d.dds") ==
              "FittingRoom\\skins\\Fair\\ube\\femalebody_1_d.dds");
        // A slot already wearing pack B still matches by name, so switching
        // packs needs no memory of the original path.
        CHECK(Match(pack, "FittingRoom\\skins\\Bijin\\femalebody_1_msn.dds") ==
              "FittingRoom\\skins\\Fair\\femalebody_1_msn.dds");
        // ⚠ THE 3BA ETC SHAPES ARE LEFT ALONE. femalebody_etc_v2_1.dds is not
        // in this pack, so the genital shapes keep their own art. This is the
        // whole reason the feature is armour overrides and not slot masks.
        CHECK(Match(pack, "textures\\actors\\character\\female\\femalebody_etc_v2_1.dds").empty());
        CHECK(Match(pack, "").empty());
        CHECK(Match(pack, "textures\\actors\\character\\female\\femalehands_1.dds").empty());
    }
    {  // The node name rule, measured from skee's OverrideApplicator::Apply.
        // One real geometry: both keys, the empty one because that is what
        // skee looks up at attach on a single-geometry addon.
        const auto one = NodeKeys("artHands", 1);
        CHECK(one.size() == 2);
        CHECK(one[0] == "artHands");
        CHECK(one[1].empty());
        // More than one: the name alone.
        const auto many = NodeKeys("3BA", 3);
        CHECK(many.size() == 1);
        CHECK(many[0] == "3BA");
        // Overlay clones do not count as real geometry, and the caller uses
        // this to arrive at the count above.
        CHECK(IsOverlayNode("Body [Ovl0]"));
        CHECK(IsOverlayNode("Hands [SOvl0]"));
        CHECK(!IsOverlayNode("artHands"));
        CHECK(!IsOverlayNode("3BA_Vagina"));
    }
    {  // The 'SKIN' record round trips, and refuses what it cannot use.
        using namespace OS;
        std::vector<SkinRow> rows;
        SkinRow              player;
        player.npcMod    = "Skyrim.esm";
        player.npcLocal  = 0x7;
        player.skin.pack = "Fair Skin";
        player.skin.written.push_back(
            Written{ "Skyrim.esm", 0xD64, "Skyrim.esm", 0xD67, "3BA", 0,
                     "textures\\actors\\character\\female\\femalebody_1.dds" });
        player.skin.written.push_back(
            Written{ "Skyrim.esm", 0xD64, "Skyrim.esm", 0xD67, "3BA", 1,
                     "textures\\actors\\character\\female\\femalebody_1_msn.dds" });
        // The empty-name twin of a one-geometry addon, and an original that
        // was itself empty: both are rows, neither is a fault.
        player.skin.written.push_back(Written{ "Skyrim.esm", 0xD64, "Skyrim.esm", 0xD67, "", 7, "" });
        rows.push_back(player);
        SkinRow follower;
        follower.npcMod   = "Serana.esp";
        follower.npcLocal = 0x1234;
        // A pack of "" with rows is legal: Default pressed, removal pending.
        follower.skin.pack = "";
        rows.push_back(follower);

        const auto           bytes = EncodeSkinRows(rows);
        std::vector<SkinRow> out;
        CHECK(DecodeSkinRows(bytes, out));
        CHECK(out == rows);

        // Empty in, empty out, and it round trips.
        std::vector<SkinRow> none;
        CHECK(DecodeSkinRows(EncodeSkinRows(none), out));
        CHECK(out.empty());

        // Truncated: refused, and the output is left as it was.
        out = rows;
        std::vector<std::byte> cut{ bytes.begin(), bytes.end() - 3 };
        CHECK(!DecodeSkinRows(cut, out));
        CHECK(out == rows);

        // Trailing garbage: refused.
        auto extra = bytes;
        extra.push_back(std::byte{ 0 });
        CHECK(!DecodeSkinRows(extra, out));

        // An actor row with no plugin name cannot find its actor: refused.
        std::vector<SkinRow> bad = rows;
        bad[0].npcMod.clear();
        CHECK(!DecodeSkinRows(EncodeSkinRows(bad), out));

        // A written row with no armour plugin cannot be taken off: refused.
        bad = rows;
        bad[0].skin.written[0].armorMod.clear();
        CHECK(!DecodeSkinRows(EncodeSkinRows(bad), out));

        // A slot the texture set cannot hold: refused.
        bad = rows;
        bad[0].skin.written[0].slot = 8;
        CHECK(!DecodeSkinRows(EncodeSkinRows(bad), out));

        // Rows are found by their KEY, whatever original they carry.
        const Written probe{ "Skyrim.esm", 0xD64, "Skyrim.esm", 0xD67, "3BA", 1, "" };
        CHECK(FindKey(rows[0].skin.written, probe) != nullptr);
        CHECK(FindKey(rows[0].skin.written, probe)->original ==
              "textures\\actors\\character\\female\\femalebody_1_msn.dds");
        const Written miss{ "Skyrim.esm", 0xD64, "Skyrim.esm", 0xD67, "3BA", 2, "" };
        CHECK(FindKey(rows[0].skin.written, miss) == nullptr);

        // A count past the cap is refused before anything is allocated.
        std::vector<std::byte> huge;
        detail::PutU32(huge, kMaxSkinRows + 1);
        CHECK(!DecodeSkinRows(huge, out));
    }
    {  // v2: the head rows ride after the armour rows, per actor, and a v1
        // record (no head list) still reads.
        using namespace OS;
        std::vector<SkinRow> rows;
        SkinRow              player;
        player.npcMod    = "Skyrim.esm";
        player.npcLocal  = 0x7;
        player.skin.pack = "Sayble 4K";
        player.skin.written.push_back(
            Written{ "Skyrim.esm", 0xD64, "Skyrim.esm", 0xD67, "Softbody", 0,
                     "textures\\!UBE\\Body\\femalebody_1_d.dds" });
        // The face by its part's name, its normal, and an overlay clone's
        // normal: three head rows, one with an empty original.
        player.skin.head.push_back(
            HeadWritten{ "00UBE_FemaleHead", 0, "data\\TEXTURES\\!UBE\\Head\\femalehead_d.dds" });
        player.skin.head.push_back(
            HeadWritten{ "00UBE_FemaleHead", 1, "data\\TEXTURES\\!UBE\\Head\\femalehead_n.dds" });
        player.skin.head.push_back(HeadWritten{ "Face [Ovl0]", 1, "" });
        rows.push_back(player);
        SkinRow follower;
        follower.npcMod   = "Serana.esp";
        follower.npcLocal = 0x1234;
        follower.skin.head.push_back(HeadWritten{ "FemaleHeadNord", 0, "x.dds" });
        rows.push_back(follower);

        const auto           bytes = EncodeSkinRows(rows);
        std::vector<SkinRow> out;
        CHECK(DecodeSkinRows(bytes, out));
        CHECK(out == rows);
        // ⚠ THE ENCODER WRITES THE NEWEST, so these bytes are v3 and reading
        // them as v2 leaves the bare count unplaced. Refused for the same
        // reason a v2 record read as v1 is: trailing bytes are a record this
        // reader does not understand, never a record to half adopt.
        CHECK(!DecodeSkinRows(bytes, out, kSkinLayoutV2));
        CHECK(!DecodeSkinRows(bytes, out, kSkinLayoutV1));
        // A layout this build does not know: refused.
        CHECK(!DecodeSkinRows(bytes, out, kSkinLayout + 1));
        CHECK(!DecodeSkinRows(bytes, out, 0));

        // A v1 record, hand built exactly as the v1 encoder wrote it (no head
        // count after the armour rows), reads under v1 with empty head lists.
        std::vector<std::byte> v1;
        detail::PutU32(v1, 1);
        detail::PutStr(v1, "Skyrim.esm");
        detail::PutU32(v1, 0x7);
        detail::PutStr(v1, "Sayble 4K");
        detail::PutU32(v1, 1);
        detail::PutStr(v1, "Skyrim.esm");
        detail::PutU32(v1, 0xD64);
        detail::PutStr(v1, "Skyrim.esm");
        detail::PutU32(v1, 0xD67);
        detail::PutStr(v1, "Softbody");
        detail::PutU8(v1, 0);
        detail::PutStr(v1, "textures\\!UBE\\Body\\femalebody_1_d.dds");
        CHECK(DecodeSkinRows(v1, out, kSkinLayoutV1));
        CHECK(out.size() == 1);
        CHECK(out[0].skin.pack == "Sayble 4K");
        CHECK(out[0].skin.written.size() == 1);
        CHECK(out[0].skin.head.empty());
        // The same bytes under v2 want a head count that is not there.
        CHECK(!DecodeSkinRows(v1, out, kSkinLayoutV2));

        // A head row with no node name could never be taken off: refused.
        std::vector<SkinRow> bad = rows;
        bad[0].skin.head[0].node.clear();
        CHECK(!DecodeSkinRows(EncodeSkinRows(bad), out));
        // A slot the texture set cannot hold: refused.
        bad = rows;
        bad[0].skin.head[0].slot = 8;
        CHECK(!DecodeSkinRows(EncodeSkinRows(bad), out));

        // Head rows are found by (node, slot), whatever original they carry.
        const HeadWritten probe{ "00UBE_FemaleHead", 1, "" };
        CHECK(FindKey(rows[0].skin.head, probe) != nullptr);
        CHECK(FindKey(rows[0].skin.head, probe)->original ==
              "data\\TEXTURES\\!UBE\\Head\\femalehead_n.dds");
        CHECK(FindKey(rows[0].skin.head, HeadWritten{ "00UBE_FemaleHead", 2, "" }) == nullptr);
        CHECK(FindKey(rows[0].skin.head, HeadWritten{ "Face [Ovl1]", 1, "" }) == nullptr);

        // ---- v3: the bare-skin rows ride after the head rows -----------------
        //
        // ⚠ A body nobody is wearing is written through the same node channel
        // the face is, so it carries HeadWritten and needs a list of its own
        // (SkinPlan.h says why it is not more head rows).
        {
            std::vector<SkinRow> v3rows = rows;
            v3rows[0].skin.bare.push_back(
                HeadWritten{ "FemaleBody [Body]", 0,
                             "data\\TEXTURES\\!UBE\\Body\\femalebody_1_d.dds" });
            v3rows[0].skin.bare.push_back(
                HeadWritten{ "FemaleBody [Body]", 1,
                             "data\\TEXTURES\\!UBE\\Body\\femalebody_1_n.dds" });
            // A hand with an original that was itself empty is a row, as above.
            v3rows[0].skin.bare.push_back(HeadWritten{ "FemaleHands [Hands]", 0, "" });

            const auto           v3bytes = EncodeSkinRows(v3rows);
            std::vector<SkinRow> v3out;
            CHECK(DecodeSkinRows(v3bytes, v3out));
            CHECK(v3out == v3rows);
            CHECK(DecodeSkinRows(v3bytes, v3out, kSkinLayoutV3));
            CHECK(v3out == v3rows);

            // ⚠⚠ AND A v2 RECORD STILL READS, which is the whole point of
            // taking the version rather than assuming the newest: a save
            // written before the bare list existed carries no bare count, and
            // throwing it away would strand every override this mod wrote on
            // that character with no list to take them off by.
            std::vector<std::byte> v2;
            detail::PutU32(v2, 1);
            detail::PutStr(v2, "Skyrim.esm");
            detail::PutU32(v2, 0x7);
            detail::PutStr(v2, "Sayble 4K");
            detail::PutU32(v2, 0);  // no armour rows
            detail::PutU32(v2, 1);  // one head row
            detail::PutStr(v2, "00UBE_FemaleHead");
            detail::PutU8(v2, 0);
            detail::PutStr(v2, "data\\TEXTURES\\!UBE\\Head\\femalehead_d.dds");
            CHECK(DecodeSkinRows(v2, v3out, kSkinLayoutV2));
            CHECK(v3out.size() == 1);
            CHECK(v3out[0].skin.head.size() == 1);
            CHECK(v3out[0].skin.bare.empty());
            // The same bytes under v3 want a bare count that is not there.
            CHECK(!DecodeSkinRows(v2, v3out, kSkinLayoutV3));

            // A bare row with no node name could never be taken off: refused.
            std::vector<SkinRow> bare = v3rows;
            bare[0].skin.bare[0].node.clear();
            CHECK(!DecodeSkinRows(EncodeSkinRows(bare), v3out));
            // A slot the texture set cannot hold: refused.
            bare = v3rows;
            bare[0].skin.bare[0].slot = 8;
            CHECK(!DecodeSkinRows(EncodeSkinRows(bare), v3out));

            // Bare rows are found by (node, slot), the same key the head uses.
            CHECK(FindKey(v3rows[0].skin.bare, HeadWritten{ "FemaleBody [Body]", 1, "" }) !=
                  nullptr);
            CHECK(FindKey(v3rows[0].skin.bare, HeadWritten{ "FemaleBody [Body]", 2, "" }) ==
                  nullptr);
            // ⚠ AND THE TWO LISTS DO NOT SEE EACH OTHER. A body node looked up
            // in the head list must miss, or the head-build repaint and the
            // rival scan would both start counting a torso as a face.
            CHECK(FindKey(v3rows[0].skin.head, HeadWritten{ "FemaleBody [Body]", 0, "" }) ==
                  nullptr);
        }

        // The head's match is the body's match: by file name, so the pack's
        // femalehead_d.dds finds the UBE face slot spelled with the data root.
        Pack pack{ "Sayble 4K",
                   { { "femalehead_d.dds", "FittingRoom\\skins\\Sayble 4K\\!UBE\\Head\\femalehead_d.dds" },
                     { "femalehead_n.dds", "FittingRoom\\skins\\Sayble 4K\\!UBE\\Head\\femalehead_n.dds" } } };
        CHECK(Match(pack, "data\\TEXTURES\\!UBE\\Head\\femalehead_d.dds") ==
              "FittingRoom\\skins\\Sayble 4K\\!UBE\\Head\\femalehead_d.dds");
        // The mouth, the eyes and the tint composite's slot name nothing the
        // pack carries, so a head walk with no feature filter leaves them alone.
        CHECK(Match(pack, "Textures\\!COR\\Mouth\\MouthMap_d.dds").empty());
        CHECK(Match(pack, "textures\\actors\\character\\eyes\\eyebrown.dds").empty());
        CHECK(Match(pack, "data\\TEXTURES\\!UBE\\Head\\femalehead_sk.dds").empty());
    }

    {  // The skin card's scene (OS-212): the character's parts, kind kSkin,
       // the pack's files as by-name swaps on every root, and the default as
       // the same figure with nothing swapped.
        using namespace OS;
        const std::vector<std::string> parts{ "!UBE\\Body\\femalebody_tangent_1.nif",
                                              "!UBE\\Head\\femaleHead_tangent.nif",
                                              "!UBE\\Hands\\femalehands_tangent_1.nif",
                                              "!UBE\\Feet\\femalefeet_tangent_1.nif" };
        Pack pack{ "Sayble 4K",
                   { { "femalebody_1_d.dds", "FittingRoom\\skins\\Sayble 4K\\!UBE\\Body\\femalebody_1_d.dds" },
                     { "femalebody_1_n.dds", "FittingRoom\\skins\\Sayble 4K\\!UBE\\Body\\femalebody_1_n.dds" },
                     { "femalehead_d.dds", "FittingRoom\\skins\\Sayble 4K\\!UBE\\Head\\femalehead_d.dds" } } };
        const auto id = SkinCardScene::Build(parts, &pack);
        CHECK(id.kind == PreviewGrid::SceneKind::kSkin);
        CHECK(id.editorId == "skin:Sayble 4K");
        CHECK(id.modelPaths == parts);
        CHECK(id.mannequinPathCount == 0 && id.morphPathCount == 0 &&
              id.dataRootedPathCount == 0);
        CHECK(id.swaps.size() == parts.size());
        for (const auto& list : id.swaps) {
            CHECK(list.size() == 3);
            // std::map order: by lower name.
            CHECK(list[0].whenTex0Name == "femalebody_1_d.dds");
            CHECK(list[0].texPaths[0] ==
                  "FittingRoom\\skins\\Sayble 4K\\!UBE\\Body\\femalebody_1_d.dds");
            CHECK(list[2].whenTex0Name == "femalehead_d.dds");
            CHECK(list[0].geomIndex == 0);  // never consulted for a by-name entry
        }
        // The body root's shape reads femalebody_1_d.dds: the body file. The
        // head root's shape reads femalehead_d.dds: the head file. A 3BA etc
        // shape reads femalebody_etc_v2_1.dds: nothing.
        CHECK(PreviewGrid::SwapFor(id.swaps[0], 0, "femalebody_1_d.dds") == &id.swaps[0][0]);
        CHECK(PreviewGrid::SwapFor(id.swaps[1], 0, "femalehead_d.dds") == &id.swaps[1][2]);
        CHECK(PreviewGrid::SwapFor(id.swaps[0], 0, "femalebody_etc_v2_1.dds") == nullptr);
        // Two packs are two keys; the default is a third; the same pack is
        // one key however many frames build it.
        Pack other = pack;
        other.id   = "Tuff Dragon";
        const auto dflt = SkinCardScene::Build(parts, nullptr);
        CHECK(dflt.kind == PreviewGrid::SceneKind::kSkin);
        CHECK(dflt.editorId == "skin:");
        CHECK(dflt.swaps.empty());
        CHECK(PreviewGrid::DiskKeyFor(id) ==
              PreviewGrid::DiskKeyFor(SkinCardScene::Build(parts, &pack)));
        CHECK(PreviewGrid::DiskKeyFor(id) !=
              PreviewGrid::DiskKeyFor(SkinCardScene::Build(parts, &other)));
        CHECK(PreviewGrid::DiskKeyFor(id) != PreviewGrid::DiskKeyFor(dflt));
        // No parts: no roots and no swaps, a scene the card draws as a cross.
        const auto none = SkinCardScene::Build({}, &pack);
        CHECK(none.modelPaths.empty() && none.swaps.empty());
    }

    {  // Which packs fit this character (OS-215): a pack that carries none of
       // the names the live skin reads would change nothing, so it is not
       // offered; one name is enough; unmeasured offers everything.
        Pack ube{ "Sayble 4K",
                  { { "femalebody_1_d.dds", "FittingRoom\\skins\\Sayble 4K\\!UBE\\Body\\femalebody_1_d.dds" },
                    { "femalehead_d.dds", "FittingRoom\\skins\\Sayble 4K\\!UBE\\Head\\femalehead_d.dds" } } };
        Pack cbbe{ "Fair Skin",
                   { { "femalebody_1.dds", "FittingRoom\\skins\\Fair Skin\\femalebody_1.dds" },
                     { "femalebody_1_msn.dds", "FittingRoom\\skins\\Fair Skin\\femalebody_1_msn.dds" } } };
        Pack headOnly{ "Face Pack",
                       { { "femalehead_d.dds", "FittingRoom\\skins\\Face Pack\\femalehead_d.dds" } } };
        const std::vector<std::string> ubeNames{ "femalebody_1_d.dds", "femalebody_1_n.dds",
                                                 "femalehead_d.dds", "femalehead_n.dds",
                                                 "femalehead_sk.dds", "mouthmap_d.dds" };
        const std::vector<std::string> cbbeNames{ "femalebody_1.dds", "femalebody_1_msn.dds",
                                                  "femalebody_1_s.dds", "femalehead.dds",
                                                  "femalehead_msn.dds" };
        CHECK(Fits(ube, ubeNames));
        CHECK(!Fits(ube, cbbeNames));
        CHECK(Fits(cbbe, cbbeNames));
        CHECK(!Fits(cbbe, ubeNames));
        // One name is enough: a head-only pack fits the UBE face.
        CHECK(Fits(headOnly, ubeNames));
        CHECK(!Fits(headOnly, cbbeNames));
        // Case: the live names arrive lower-cased from FileName, but a caller
        // that did not fold still matches.
        CHECK(Fits(ube, { "FemaleBody_1_D.DDS" }));
        // Unmeasured: everything fits.
        CHECK(Fits(ube, {}));
        CHECK(Fits(cbbe, {}));
        // An empty pack fits nothing that was measured.
        CHECK(!Fits(Pack{ "Empty", {} }, ubeNames));
    }
    {  // What the game's own skin is called (OS-216): the Mod Organizer folder
       // when the real path shows one, else the family folder, else nothing.
        CHECK(DefaultNameFromPath(
                  "C:\\Games\\Nolvus\\Instances\\Nolvus Awakening\\MODS\\mods\\Sayble 4K UBE\\textures\\!UBE\\Body\\femalebody_1_d.dds",
                  "data\\TEXTURES\\!UBE\\Body\\femalebody_1_d.dds") == "Sayble 4K UBE");
        // Any case for the segment, the mod's own spelling for the name.
        CHECK(DefaultNameFromPath("d:\\mo2\\Mods\\Bijin Skin\\Textures\\actors\\character\\female\\femalebody_1.dds",
                                  "textures\\actors\\character\\female\\femalebody_1.dds") == "Bijin Skin");
        // No mods folder (Vortex, manual, or the VFS answering with the virtual
        // path): the family folder under textures, without its sorting prefix.
        CHECK(DefaultNameFromPath("C:\\Games\\Skyrim\\Data\\textures\\!UBE\\Body\\femalebody_1_d.dds",
                                  "data\\TEXTURES\\!UBE\\Body\\femalebody_1_d.dds") == "UBE");
        CHECK(DefaultNameFromPath("", "Textures\\_Coco\\Body\\femalebody_1.dds") == "Coco");
        // The vanilla tree names nothing.
        CHECK(DefaultNameFromPath("C:\\Games\\Skyrim\\Data\\textures\\actors\\character\\female\\femalebody_1.dds",
                                  "textures\\actors\\character\\female\\femalebody_1.dds").empty());
        CHECK(DefaultNameFromPath("", "actors\\character\\female\\femalebody_1.dds").empty());
        // A slot path with no textures segment at all: nothing.
        CHECK(DefaultNameFromPath("", "femalebody_1.dds").empty());
        CHECK(DefaultNameFromPath("", "").empty());
        // A mods segment with nothing after it falls through to the fallback.
        CHECK(DefaultNameFromPath("C:\\x\\mods\\", "textures\\!UBE\\Body\\femalebody_1_d.dds") == "UBE");
    }

    {  // ---- SkinImport: who really owns a file, and where a link goes ----
        using namespace OS::SkinImport;

        // ⚠ THE NOLVUS LAYOUT IS WHY THIS CANNOT TAKE THE FIRST "\mods\": the
        // instance keeps its mods under MODS\mods\, so the first occurrence
        // names the folder called "mods". The one FOLLOWED BY a mod root is the
        // one that means something, which is the rule DefaultNameFromPath
        // already had to learn.
        const std::string nolvus =
            "C:\\Games\\Nolvus\\Instances\\Nolvus Awakening\\MODS\\mods\\"
            "UBE 2.0 Jada 8.1 Skin\\textures\\actors\\character\\female\\femalebody_1.dds";
        CHECK(OwnerOf(nolvus).mod == "UBE 2.0 Jada 8.1 Skin");
        CHECK(OwnerOf(nolvus).relative ==
              "textures\\actors\\character\\female\\femalebody_1.dds");
        CHECK(OwnerOf(nolvus).Ok());
        CHECK(ModsRootOf(nolvus) ==
              "C:\\Games\\Nolvus\\Instances\\Nolvus Awakening\\MODS\\mods\\");

        // A plain Mod Organizer install: one "\mods\" and it is the right one.
        const std::string plain =
            "D:\\MO2\\mods\\Bijin Skin\\textures\\actors\\character\\female\\femalebody_1_msn.dds";
        CHECK(OwnerOf(plain).mod == "Bijin Skin");
        CHECK(OwnerOf(plain).relative ==
              "textures\\actors\\character\\female\\femalebody_1_msn.dds");
        CHECK(ModsRootOf(plain) == "D:\\MO2\\mods\\");

        // ⚠⚠ VORTEX AND A MANUAL INSTALL HAVE NO SUCH STRUCTURE, and the
        // feature has to SAY it found nothing rather than invent a mod name.
        CHECK(!OwnerOf("C:\\Games\\Skyrim\\Data\\textures\\actors\\character\\female\\femalebody_1.dds").Ok());
        CHECK(ModsRootOf("C:\\Games\\Skyrim\\Data\\textures\\actors\\character\\female\\femalebody_1.dds").empty());
        CHECK(!OwnerOf("").Ok());
        CHECK(ModsRootOf("").empty());
        // A mods segment with no mod root after it is not a mod either, and a
        // mod whose file is not under textures\ cannot be matched by relative
        // path, so it is not an owner this feature can use.
        CHECK(!OwnerOf("C:\\x\\mods\\").Ok());
        CHECK(!OwnerOf("C:\\x\\mods\\SomeMod\\meshes\\a.nif").Ok());

        // Where to look for the same file inside a rival mod.
        CHECK(CandidatePath("D:\\MO2\\mods\\", "Fair Skin Complexion",
                            "textures\\actors\\character\\female\\femalebody_1.dds") ==
              "D:\\MO2\\mods\\Fair Skin Complexion\\textures\\actors\\character\\female\\femalebody_1.dds");
        // A mods root without its trailing separator joins the same way.
        CHECK(CandidatePath("D:\\MO2\\mods", "Fair Skin Complexion",
                            "textures\\actors\\character\\female\\femalebody_1.dds") ==
              CandidatePath("D:\\MO2\\mods\\", "Fair Skin Complexion",
                            "textures\\actors\\character\\female\\femalebody_1.dds"));

        // Where the hard link lands. The leading textures\ is dropped because
        // the skins directory already sits under one, and the REST of the
        // source path is mirrored so two source folders holding the same leaf
        // name cannot collide into one pack entry.
        const std::string skins =
            "D:\\MO2\\mods\\Fitting Room\\textures\\FittingRoom\\skins";
        CHECK(LinkTarget(skins, "Fair Skin Complexion",
                         "textures\\actors\\character\\female\\femalebody_1.dds") ==
              "D:\\MO2\\mods\\Fitting Room\\textures\\FittingRoom\\skins\\"
              "Fair Skin Complexion\\actors\\character\\female\\femalebody_1.dds");
        // A trailing separator on the skins directory changes nothing.
        CHECK(LinkTarget(skins + "\\", "Fair Skin Complexion",
                         "textures\\actors\\character\\female\\femalebody_1.dds") ==
              LinkTarget(skins, "Fair Skin Complexion",
                         "textures\\actors\\character\\female\\femalebody_1.dds"));
        // Two rival files with the same leaf name land apart, which is the
        // whole reason the subdirectories are mirrored rather than flattened.
        CHECK(LinkTarget(skins, "Rival", "textures\\actors\\character\\female\\femalebody_1.dds") !=
              LinkTarget(skins, "Rival", "textures\\actors\\character\\male\\femalebody_1.dds"));

        // ⚠⚠ THE ROUND TRIP IS THE POINT OF THE LAYOUT. A file linked to that
        // target has to read back as a pack named after the MOD, or the grid
        // draws it under the wrong name and SkinPlan::Match never finds it.
        // Feed the tail back through the two functions that decide.
        const auto target = LinkTarget(skins, "Fair Skin Complexion",
                                       "textures\\actors\\character\\female\\femalebody_1.dds");
        const auto at     = target.find("FittingRoom\\skins\\");
        CHECK(at != std::string::npos);
        const auto overridePath = target.substr(at);
        CHECK(PackIdOf(overridePath) == "Fair Skin Complexion");
        CHECK(FileName(overridePath) == "femalebody_1.dds");
    }

    {  // Our own staged files, which can never name what the load order gives.
        CHECK(IsOwnFile("FittingRoom\\skins\\Fair\\femalebody_1.dds"));
        CHECK(IsOwnFile("textures\\fittingroom\\SKINS\\Bijin\\femalebody_1_d.dds"));
        CHECK(!IsOwnFile("textures\\actors\\character\\female\\femalebody_1.dds"));
        CHECK(!IsOwnFile(""));
    }
    {  // ---- what a pack change does to one stored row ----
        //
        // The field case this pins: a race switch detaches the gear, a
        // clearing apply lands in the gap, and every row went with it. The
        // rows are the only record of the original path, so a detached one
        // has to survive.
        Pack pack;
        pack.id    = "Sayble 4K";
        pack.files = { { "femalebody_1_d.dds",
                         "FittingRoom\\skins\\Sayble 4K\\!UBE\\Body\\femalebody_1_d.dds" } };

        Written body;
        body.armorMod   = "Skyrim.esm";
        body.armorLocal = 0x1;
        body.addonMod   = "Skyrim.esm";
        body.addonLocal = 0x2;
        body.node       = "UMBRAEL UBE";
        body.slot       = 0;
        body.original   = "textures\\!UBE\\Body\\femalebody_1_d.dds";

        // The new pack carries this file name, so the override moves whether
        // or not the piece is on the actor.
        CHECK(FateOf(&pack, body, true) == RowFate::Move);
        CHECK(FateOf(&pack, body, false) == RowFate::Move);

        // Clearing while dressed still clears, which is what
        // replace-on-apply's own Apply(player, "") depends on.
        CHECK(FateOf(nullptr, body, true) == RowFate::Remove);

        // ⚠⚠ AND CLEARING WHILE THE PIECE IS OFF HOLDS. Removing here would
        // drop the one record of what was under the override with nothing to
        // write it back onto.
        CHECK(FateOf(nullptr, body, false) == RowFate::Hold);

        // A pack that carries no file of this name is the same as no pack.
        Pack other;
        other.id    = "Head Only";
        other.files = { { "femalehead_d.dds", "FittingRoom\\skins\\Head Only\\femalehead_d.dds" } };
        CHECK(FateOf(&other, body, true) == RowFate::Remove);
        CHECK(FateOf(&other, body, false) == RowFate::Hold);

        // A row that never recorded an original holds nothing worth keeping,
        // so there is no gap for it to survive.
        Written blank = body;
        blank.original.clear();
        CHECK(FateOf(nullptr, blank, false) == RowFate::Remove);
        CHECK(FateOf(&pack, blank, false) == RowFate::Remove);

        // ⚠⚠ AND THE HEAD ASKS THE SAME QUESTION THROUGH THE SAME DOOR. It has
        // the identical gap and only the body was fixed first: a race switch
        // rebuilds the face, so the named geometry is away for a moment and a
        // clear landing there used to drop every head row onto nothing (field
        // r85: `head 0 moved, 9 removed ... over 0 worn addon(s)`, then
        // `head slots none` on every apply after it). "Present" means the
        // geometry is on the actor rather than the addon being attached, which
        // is why this overload takes the original and a boolean.
        Pack headPack;
        headPack.id    = "Sayble 4K";
        headPack.files = { { "femalehead_d.dds",
                             "FittingRoom\\skins\\Sayble 4K\\!UBE\\Head\\femalehead_d.dds" } };
        const std::string face = "textures\\!UBE\\Head\\femalehead_d.dds";
        CHECK(FateOf(&headPack, face, true) == RowFate::Move);
        CHECK(FateOf(&headPack, face, false) == RowFate::Move);
        CHECK(FateOf(nullptr, face, true) == RowFate::Remove);
        CHECK(FateOf(nullptr, face, false) == RowFate::Hold);

        // ⚠ A PRESET'S EXPORTED FACE MATCHES NOTHING, and that is the neck
        // seam rather than a bug in this decision. No pack is called
        // 'FR_Umbrael 19.dds', so the face keeps what it had while the body
        // changes.
        const std::string exported = "Textures\\CharGen\\Exported\\FR_Umbrael 19.dds";
        CHECK(Match(headPack, exported).empty());
        CHECK(FateOf(&headPack, exported, true) == RowFate::Remove);
        CHECK(FateOf(&headPack, exported, false) == RowFate::Hold);
    }

    // ---- the other sex's body diffuse, the skin gate's second key ----------
    //
    // ⚠⚠ THE FIELD CASE. 'Zhizhen Female Skin - UBE' ships the female half and
    // 'Zhizhen Female Skin - UBE Penis Patch' ships ONLY malebody_1_d.dds and
    // malebody_1_n.dds. The patch replaces a body diffuse of a UBE futa
    // character and the rival scan threw it away for replacing the wrong one,
    // so a switch to Zhizhen could never carry the schlong, while Loona and
    // Jada, which ship both halves in one folder, worked.
    {
        // Both directions, because a male character's anchor is the male one.
        CHECK(SexSiblingOf("textures\\!UBE\\Body\\femalebody_1_d.dds") ==
              "textures\\!UBE\\Body\\malebody_1_d.dds");
        CHECK(SexSiblingOf("textures\\!UBE\\Body\\malebody_1_d.dds") ==
              "textures\\!UBE\\Body\\femalebody_1_d.dds");

        // ⚠⚠ THE FILE NAME ONLY, NEVER THE DIRECTORY. A vanilla-path body sits
        // under ...\\character\\female\\, and a swap over the whole string
        // would rewrite the FOLDER and ask about a file nobody has.
        CHECK(SexSiblingOf("textures\\actors\\character\\female\\femalebody_1.dds") ==
              "textures\\actors\\character\\female\\malebody_1.dds");

        // A name carrying neither sex has no sibling, and an empty key must
        // match nothing rather than everything.
        CHECK(SexSiblingOf("textures\\!UBE\\Body\\bodyskin_d.dds").empty());
        CHECK(SexSiblingOf("").empty());
        CHECK(SexSiblingOf("textures\\!UBE\\Body\\").empty());

        // ⚠ ROUND TRIPS, so neither direction can quietly drift into the other.
        const std::string fem = "textures\\!ube\\body\\femalebody_1_d.dds";
        CHECK(SexSiblingOf(SexSiblingOf(fem)) == fem);
    }

    std::printf("SkinPlanTests: all passed\n");
    return 0;
}
