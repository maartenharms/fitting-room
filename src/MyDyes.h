#pragma once

#include "DyePalette.h"

#include <json/json.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The player's own dyes (spec 2026-08-09): ONE pack file,
// Data/SKSE/Plugins/FittingRoom/Dyes/custom.json, that DyePalette::Load
// already merges like any pack. This module is the WRITE side only; reading
// stays the loader's job, and the grid learns which dyes are the player's
// from Dye::custom.
//
// Pure: no engine types, so this compiles into the test executable, the
// DyeSchemes precedent it is shaped after.
namespace OS::MyDyes {

    // ⚠ THE NAMESPACE IS THE COLLISION RULE. Pack ids follow "pack:name" by
    // convention and no shipped pack starts with "custom:", so a custom id
    // can never lose a first-file-wins merge to a pack, and unlocks stored by
    // id can never be claimed by one.
    inline constexpr std::string_view kIdPrefix = "custom:";

    // "custom:" plus the lowercased name with the path-hostile characters
    // folded, the same fold and the same reason as DyeSchemes::FileNameFor:
    // the name is free text and the id must be stable, comparable and safe.
    [[nodiscard]] std::string IdForName(std::string_view a_name);

    // One dye, in exactly the vocabulary DyePalette::DyeFromJson reads: id,
    // name, hex always; hex2, mode, flake, gloss, sheen only when the channel
    // says them. Strength and player are the PLAYER's and are never written;
    // no rarity, so the dye is free under lore promotion, the documented
    // meaning of an absent one.
    [[nodiscard]] Json::Value DyeToJson(const Dye& a_dye);

    // The whole pack, under the same "dyes" root key the loader scans.
    [[nodiscard]] Json::Value PackToJson(const std::vector<Dye>& a_customs);

    // Replace-on-same-id, append otherwise: the schemes' save rule.
    void UpsertById(std::vector<Dye>& a_into, const Dye& a_dye);

    // Write the pack file whole. False on I/O failure.
    [[nodiscard]] bool WritePack(const std::vector<Dye>& a_customs);

    // The pack's own dyes, parsed out of pack TEXT with the loader's parser so
    // the read and the write can never drift, and stamped custom because this
    // text IS the custom pack (the loader stamps the same mark from the
    // filename, the only place either of them can know it).
    //
    // ⚠⚠ NULLOPT MEANS "A PACK EXISTS AND I COULD NOT READ IT", WHICH IS NOT
    // AN EMPTY PACK. Every caller writes the whole file back, so handing an
    // empty list back for an unreadable one is a silent delete of everything
    // in it. The split follows DyesFromJson's: a malformed ENTRY costs that
    // entry and the file survives; a malformed FILE is refused whole.
    [[nodiscard]] std::optional<std::vector<Dye>> CustomsFromPackText(
        std::string_view a_text);

    // The pack as it is on disk right now. An absent file is an empty pack, a
    // present one that will not read is nullopt, on CustomsFromPackText's
    // meaning.
    [[nodiscard]] std::optional<std::vector<Dye>> LoadPack();

    // The UI's two verbs. Both read the current customs off DISK, mutate, and
    // write the file whole. Neither reloads the palette: DyePalette::Load is
    // main-thread-only by its own contract, so the caller marshals the reload.
    //
    // ⚠⚠ DISK, NOT DyePalette::Snapshot(), AND THAT IS THE WHOLE POINT. These
    // used to derive the list from the palette, which only gains a newly saved
    // dye once the caller's marshalled reload has drained. Two saves inside one
    // reload window therefore wrote a pack holding only the second, and the
    // first was gone from disk with nothing logged. The file is the
    // authoritative copy and it is not subject to that timing; the palette
    // reload now only exists to refresh what the GRID shows, which is what it
    // was ever for.
    //
    // False from either can also mean the pack was there and unreadable, so a
    // refusal is never proof the write merely failed.
    [[nodiscard]] bool Save(const Dye& a_dye);
    [[nodiscard]] bool Remove(std::string_view a_id);

}  // namespace OS::MyDyes
