# Fitting Room

Change how your gear looks without changing what it does. Build an outfit in an in-game editor, then wear it as a pure appearance layer over your real equipment, exactly like transmog in The Elder Scrolls Online. Your stats, enchantments and weight never change; only the look does.

SKSE plugin for Skyrim Special Edition **1.5.97** and Anniversary Edition **1.6.1130+**. One build covers both.

## Features

- ESO-style transmog: your equipped gear keeps every stat and enchantment; only its appearance changes to the outfit you choose.
- Save up to six named outfits and switch between them in game.
- An in-game editor with live preview, so you build a look and see it on your character straight away. Controller-first, and vanilla-styled through the FLICK UI framework.
- Nothing is added to or removed from your inventory. It is a render-layer override, so armor skill experience and equip conflicts stay honest to your real gear.
- Body-aware fit detection: it reads the armature of what you own and offers the pieces that fit your body.
- Auto-discovered presets from the gear you already have, plus a showcase preset library to import from.
- Per-save outfit collections: each save owns its outfits, with a shared library as the seed.
- Optional lore item, the Seamstone: a carryable Alteration focus sold by Farengar in Dragonsreach that opens the editor. Optional, and you can turn it off.
- Optional gold cost for creating outfits, just like in ESO. Toggle it off to build for free.

## Requirements

- Skyrim SE **1.5.97** or AE **1.6.1130+**. VR and older runtimes are refused.
- [SKSE64](https://skse.silverlock.org/) for your runtime
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444) (the SE or AE database to match)
- [FLICK](https://www.nexusmods.com/skyrimspecialedition/mods/181603) by Fuzzles: the in-game UI framework the editor runs on. On AE, install FLICK's AE build.
- Optional: the Seamstone lore item (a small ESP, offered by the installer). Without it, open the editor by its hotkey.
- Optional: **Favorite Misc Items**, so you can favorite the Seamstone and open Fitting Room from your favorites hotkey.

## Settings

Open the editor with **Y** in your inventory by default (rebindable), or with the optional Seamstone. Configure it from the FLICK panel (pick **Fitting Room**) or `Data/SKSE/Plugins/FittingRoom.ini` (created on first run; under MO2 it lands in the overwrite folder). The editor hotkey, gold cost, Seamstone requirement, UI size and other toggles live there.

## Compatibility

- **Appearance only, no inventory or equip scripts**, so it does not fight armor mods, followers, or quests that check what you have equipped.
- Works with custom bodies and armor; it reads the fit of what you own and shows outfits accordingly.
- Plays well with **OStim** and **Screen Archer Menu** (use Fitting Room in place of SAM's own outfit and inventory system).
- **Apparel Preview** and **Menu Studio**: built to pair. When the Fitting Room editor opens it tells Apparel Preview to clear any active preview, so you start from your true look.
- The game still sees your real gear for armor rating, enchantments and equip rules; only the rendered look changes.
- Every engine hook site is byte-verified before patching. On AE the sites resolve through RELOCATION_ID and self-disable if a byte does not match, so an unverified build fails safe rather than crashing.

## Known limitations

- **Appearance only.** Outfits change the look, not the gear. Enchantment visual effects (glow, shaders) follow your real equipped items, not the outfit shown over them.
- **No weapons or quivers yet.** Transmog covers armor, clothing and jewelry for now.
- **Anniversary Edition support is new.** The AE build is verified offline against 1.6.1170; in-game AE testing is ongoing. See [KNOWN-ISSUES.md](KNOWN-ISSUES.md).
- The editor runs on FLICK; there is no separate fallback UI in a normal install.

## Planned

On the roadmap for future updates, not in the current build:

- Follower and NPC transmog: give your followers styled looks, the same way you style your own.
- Location and weather based outfit switching: set rules so your outfit changes on its own with where you are, the weather, or the time of day.

## How it works

Fitting Room hooks the character's worn-armor render pass. It hides the slots your outfit covers, injects the outfit's 3D over them, then restores the real worn armor in the data the game reads for stats and equip conflicts. So the game keeps scoring your actual gear for skill experience and equip rules, while the screen shows the look you picked. Outfits live in SKSE co-save data, per save, with a shared JSON library as the seed; no dynamic forms, no inventory changes, no equipment scripts. Every patch site is resolved through the Address Library and byte-checked before installation; on AE the same sites resolve through RELOCATION_ID and the mid-function hooks self-disable on a byte mismatch, so an unverified build fails safe.

## Credits

- **Skyrim Outfit System** by DavidJCobb, with the SE ports by aers and MetricExpansion: the "worn is not shown" biped-override technique and the documented SE hook sites. Reimplemented from their published documentation; no code copied.
- **FLICK** by Fuzzles: the in-game UI framework the editor is built on.
- **Font Awesome**: the editor's icon set (SIL OFL).
- The SKSE team, the CommonLibSSE-NG maintainers, and the Address Library project, without which none of this exists.

## Building

CMake + vcpkg (`commonlibsse-ng`, `xbyak`, `simpleini`, `jsoncpp`, `imgui`). Run `tools/build.bat` from PowerShell; it configures, builds, runs the unit tests and deploys. The release FOMOD installer is assembled by `tools/make_fomod.sh` (metadata and installer logic live in `fomod/`).

## License

GPL-3.0; see [LICENSE](LICENSE). The vendored `extern/FUCK_API.h` (FLICK) is GPL-3.0. Attributions and the licenses of all build dependencies and bundled assets are listed in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
