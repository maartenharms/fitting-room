#include "DefaultLook.h"

#include "BuildChannel.h"
#include "NpcIdentity.h"

#include <json/json.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace OS::DefaultLook {

    namespace {
        const auto kFile = BuildChannel::DataPath("default-looks.json");

        // Same guard kMaxNpcAssignments and DefaultBody apply: a corrupt count
        // must not be able to make us allocate without bound.
        constexpr std::size_t kMaxEntries = 4096;

        std::mutex                             g_mutex;
        std::unordered_map<std::string, Entry> g_map;  // guarded by g_mutex

        // "Plugin.esp|0008F0", the same key text DefaultBody writes, so the two
        // files can be read side by side by hand.
        [[nodiscard]] std::string KeyText(const NpcKey& a_key) {
            char buf[16]{};
            std::snprintf(buf, sizeof(buf), "%06X", a_key.localFormID);
            return a_key.modName + "|" + buf;
        }

        [[nodiscard]] std::optional<std::string> KeyOf(RE::Actor* a_actor) {
            if (!a_actor) {
                return std::nullopt;
            }
            const auto key = NpcKeyFor(a_actor->GetActorBase());
            if (!key) {
                return std::nullopt;  // runtime base: no identity that survives a reload
            }
            return KeyText(*key);
        }

        [[nodiscard]] StyleRefKey* Field(Entry& a_entry, Part a_part) {
            switch (a_part) {
                case Part::kHair:       return &a_entry.hair;
                case Part::kEyes:       return &a_entry.eyes;
                case Part::kBrows:      return &a_entry.brows;
                case Part::kFacialHair: return &a_entry.facialHair;
            }
            return nullptr;
        }

        [[nodiscard]] const char* FieldName(Part a_part) {
            switch (a_part) {
                case Part::kHair:       return "hair";
                case Part::kEyes:       return "eyes";
                case Part::kBrows:      return "brows";
                case Part::kFacialHair: return "facialHair";
            }
            return "";
        }

        [[nodiscard]] StyleRefKey ReadKey(const Json::Value& a_node, const char* a_name) {
            StyleRefKey out;
            const auto& ref = a_node[a_name];
            if (!ref.isObject()) {
                return out;
            }
            out.modName     = ref.get("mod", "").asString();
            const auto& id  = ref["id"];
            out.localFormID = id.isUInt() ? id.asUInt() : 0u;
            // A half-filled reference cannot resolve to a head part, so it is
            // dropped rather than kept as a thing that fails every lookup.
            if (out.modName.empty() || out.localFormID == 0) {
                return StyleRefKey{};
            }
            return out;
        }

    }  // namespace

    const char* PartName(Part a_part) {
        switch (a_part) {
            case Part::kHair:       return "hair";
            case Part::kEyes:       return "eyes";
            case Part::kBrows:      return "brows";
            case Part::kFacialHair: return "facial hair";
        }
        return "?";
    }

    std::size_t ImportLegacyFile() {
        std::scoped_lock l(g_mutex);
        // ⚠ DOES NOT CLEAR. The caller has already reset the map for this load
        // and a clear here would be a second writer of that decision.
        std::ifstream f{ kFile };
        if (!f) {
            return 0;  // the ordinary case now: no legacy file to take over
        }
        Json::Value             root;
        Json::String            errs;
        Json::CharReaderBuilder b;
        if (!Json::parseFromStream(b, f, &root, &errs) || !root.isObject()) {
            // ⚠ REFUSED WHOLE, NOT PARTIALLY, as DefaultBody explains: a
            // half-read map is written straight back out by the next Set and
            // takes the unread half with it.
            spdlog::error("DefaultLook: '{}' is not readable JSON ({}); leaving it alone.",
                          kFile.string(), errs);
            return 0;
        }
        for (const auto& key : root.getMemberNames()) {
            if (g_map.size() >= kMaxEntries) {
                spdlog::warn("DefaultLook: more than {} entries; the rest are ignored.",
                             kMaxEntries);
                break;
            }
            const auto& node = root[key];
            if (!node.isObject()) {
                continue;
            }
            Entry e;
            e.hair  = ReadKey(node, "hair");
            e.eyes  = ReadKey(node, "eyes");
            e.brows = ReadKey(node, "brows");
            // The retired file predates facial hair, so this is always empty
            // in practice. Read anyway: FieldName already spells the key, and
            // a hand-edited file naming one should not be silently ignored.
            e.facialHair = ReadKey(node, "facialHair");
            if (!e.Empty()) {
                g_map.emplace(key, std::move(e));
            }
        }
        spdlog::info("DefaultLook: imported {} character(s) from the retired '{}'. They "
                     "belong to THIS save from now on.",
                     g_map.size(), kFile.string());
        return g_map.size();
    }

    std::vector<DefaultLookRecord> Snapshot() {
        std::scoped_lock l(g_mutex);
        std::vector<DefaultLookRecord> rows;
        rows.reserve(g_map.size());
        for (const auto& [key, e] : g_map) {
            DefaultLookRecord row;
            row.modAndId = key;
            row.hairMod  = e.hair.modName;
            row.hairId   = e.hair.localFormID;
            row.eyesMod  = e.eyes.modName;
            row.eyesId   = e.eyes.localFormID;
            row.browsMod  = e.brows.modName;
            row.browsId   = e.brows.localFormID;
            row.facialMod = e.facialHair.modName;
            row.facialId  = e.facialHair.localFormID;
            // ⚠ THE FLAG IS WRITTEN, NOT DERIVED FROM THE RGB. Black is a
            // colour a player picks on purpose, so an encoder that inferred
            // "no default" from a cleared triple would delete it on save.
            row.hairTintSet = e.hairTint.set;
            row.hairTintR   = e.hairTint.r;
            row.hairTintG   = e.hairTint.g;
            row.hairTintB   = e.hairTint.b;
            // ⚠ SORTED BY SLOT ON THE WAY OUT, for the reason the rows
            // themselves are sorted below: the list's order comes from the
            // order the player happened to pin things in, and unsorted bytes
            // make every save differ from the last for no reason at all.
            row.customHeadParts.reserve(e.customHeadParts.size());
            for (const auto& part : e.customHeadParts) {
                row.customHeadParts.push_back(DefaultLookSlotRecord{
                    part.slot, part.key.modName, part.key.localFormID });
            }
            std::sort(row.customHeadParts.begin(), row.customHeadParts.end(),
                      [](const DefaultLookSlotRecord& a, const DefaultLookSlotRecord& b) {
                          return a.slot < b.slot;
                      });
            rows.push_back(std::move(row));
        }
        // Sorted so the bytes are stable across runs: g_map is unordered, and
        // an unstable order would make every save differ from the last for no
        // reason at all.
        std::sort(rows.begin(), rows.end(),
                  [](const DefaultLookRecord& a, const DefaultLookRecord& b) {
                      return a.modAndId < b.modAndId;
                  });
        return rows;
    }

    void Restore(const std::vector<DefaultLookRecord>& a_rows) {
        std::scoped_lock l(g_mutex);
        g_map.clear();
        for (const auto& row : a_rows) {
            Entry e;
            e.hair       = StyleRefKey{ row.hairMod, row.hairId };
            e.eyes       = StyleRefKey{ row.eyesMod, row.eyesId };
            e.brows      = StyleRefKey{ row.browsMod, row.browsId };
            // Empty on a v1 record, which is exactly right: a save written
            // before facial hair names none.
            e.facialHair = StyleRefKey{ row.facialMod, row.facialId };
            // Cleared on a v1 or v2 record, which is exactly right: a save
            // written before this names no default colour, and a cleared tint
            // is the leave-their-own-colour-alone value.
            e.hairTint.set = row.hairTintSet;
            e.hairTint.r   = row.hairTintR;
            e.hairTint.g   = row.hairTintG;
            e.hairTint.b   = row.hairTintB;
            // Empty on anything older than v4, which is exactly right: a save
            // written before pinned slots names none. ⚠ THROUGH THE SETTER,
            // so the one-entry-per-slot rule holds even if a hand-made record
            // names a slot twice.
            for (const auto& part : row.customHeadParts) {
                e.SetCustomHeadPart(part.slot,
                                    StyleRefKey{ part.mod, part.id });
            }
            // ⚠ AN EMPTY ENTRY IS DROPPED, the same rule Set() follows. Kept, it
            // would answer Has() true for a look that names nothing.
            if (!e.Empty() && !row.modAndId.empty()) {
                g_map.emplace(row.modAndId, std::move(e));
            }
        }
        spdlog::info("DefaultLook: this save holds {} character default(s).", g_map.size());
    }

    void ClearAll() {
        std::scoped_lock l(g_mutex);
        g_map.clear();
    }

    Entry For(RE::Actor* a_actor) {
        const auto key = KeyOf(a_actor);
        if (!key) {
            return {};
        }
        std::scoped_lock l(g_mutex);
        const auto       it = g_map.find(*key);
        return it == g_map.end() ? Entry{} : it->second;
    }

    StyleRefKey For(RE::Actor* a_actor, Part a_part) {
        Entry       entry = For(a_actor);
        const auto* field = Field(entry, a_part);
        return field ? *field : StyleRefKey{};
    }

    bool Has(RE::Actor* a_actor, Part a_part) { return !For(a_actor, a_part).Empty(); }

    void Set(RE::Actor* a_actor, Part a_part, const StyleRefKey& a_key) {
        const auto key = KeyOf(a_actor);
        if (!key) {
            return;
        }
        {
            std::scoped_lock l(g_mutex);
            auto&            entry = g_map[*key];
            if (auto* field = Field(entry, a_part)) {
                *field = a_key;
            }
            // ⚠ AN ENTRY THAT HAS LOST ITS LAST PART IS ERASED, not left behind
            // empty. An empty entry would be written to the file, read back at
            // startup as a character with a default, and answer Has() true for
            // a look that names nothing.
            if (entry.Empty()) {
                g_map.erase(*key);
            }
            // ⚠ NO WRITE HERE ANY MORE. The co-save owns this map: the next
            // game save carries it through 'DFLK'. See the header.
        }
        if (a_key.Empty()) {
            spdlog::info("DefaultLook: '{}' default {} cleared.", *key, PartName(a_part));
        } else {
            spdlog::info("DefaultLook: '{}' default {} set to '{}'|{:06X}.", *key,
                         PartName(a_part), a_key.modName, a_key.localFormID);
        }
    }

    void Clear(RE::Actor* a_actor, Part a_part) { Set(a_actor, a_part, StyleRefKey{}); }

    StyleRefKey ForSlot(RE::Actor* a_actor, std::uint32_t a_slot) {
        return For(a_actor).CustomHeadPart(a_slot);
    }

    bool HasSlot(RE::Actor* a_actor, std::uint32_t a_slot) {
        return !ForSlot(a_actor, a_slot).Empty();
    }

    void SetSlot(RE::Actor* a_actor, std::uint32_t a_slot, const StyleRefKey& a_key) {
        const auto key = KeyOf(a_actor);
        if (!key) {
            return;
        }
        {
            std::scoped_lock l(g_mutex);
            auto&            entry = g_map[*key];
            entry.SetCustomHeadPart(a_slot, a_key);
            // ⚠ THE SAME ERASE THE FOUR KINDS GET. An entry whose last pin has
            // gone would otherwise be written out and read back as a character
            // with a default that names nothing.
            if (entry.Empty()) {
                g_map.erase(*key);
            }
        }
        if (a_key.Empty()) {
            spdlog::info("DefaultLook: '{}' pinned slot {} cleared.", *key, a_slot);
        } else {
            spdlog::info("DefaultLook: '{}' slot {} pinned to '{}'|{:06X}.", *key, a_slot,
                         a_key.modName, a_key.localFormID);
        }
    }

    void ClearSlot(RE::Actor* a_actor, std::uint32_t a_slot) {
        SetSlot(a_actor, a_slot, StyleRefKey{});
    }

    HairTint HairColour(RE::Actor* a_actor) { return For(a_actor).hairTint; }

    bool HasHairColour(RE::Actor* a_actor) { return HairColour(a_actor).set; }

    void SetHairColour(RE::Actor* a_actor, const HairTint& a_tint) {
        const auto key = KeyOf(a_actor);
        if (!key) {
            return;
        }
        {
            std::scoped_lock l(g_mutex);
            auto&            entry = g_map[*key];
            entry.hairTint         = a_tint;
            // ⚠ THE SAME ERASE Set() DOES, and for the same reason: an entry
            // that has lost its last member would be written to the co-save,
            // read back as a character who has a default, and answer Has()
            // true for a look that names nothing.
            if (entry.Empty()) {
                g_map.erase(*key);
            }
        }
        if (!a_tint.set) {
            spdlog::info("DefaultLook: '{}' default hair colour cleared.", *key);
        } else {
            spdlog::info("DefaultLook: '{}' default hair colour set to ({},{},{}).", *key,
                         a_tint.r, a_tint.g, a_tint.b);
        }
    }

}  // namespace OS::DefaultLook
