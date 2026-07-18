# Changelog

## 0.1.2 (2026-07-17)

- Fixed reading "On the Outward Art" crashing the game on some load orders. The book record in the optional lore ESP was missing the inventory art model that every vanilla readable book carries, which sent the reading menu down a rarely used fallback path. The record now matches vanilla notes exactly. Update the lore ESP from this archive if you use it.
- Outfits now apply to your first-person hands and arms. Previously only the third-person view was styled, so looking down in first person still showed your real gauntlets.
- You can now rotate the camera while the editor is open: hold the left mouse button and drag over the world (not over the editor panels). Works in the normal editor and with Screen Archer Menu's free camera. Toggle with bCameraDragWhileOpen in FittingRoom.ini if you prefer the old behavior.
- The installer now asks how the Fitting Room should open: Lore-friendly (the Seamstone opens it, and changing an outfit costs gold) or Free-form (your hotkey opens it, changes are free, no plugin). It is purely a choice of starting settings, and both install the same mod.
- New Playstyle section at the top of the settings panel with the same two presets, one click each, and the gold and Seamstone settings are now grouped together under Lore. The panel also tells you when the Seamstone requirement is doing nothing because the plugin is not installed.

## 0.1.1 (2026-07-15)

- Fixed transmog not rendering on Anniversary Edition. The worn-armor pass the mod hooks was inlined into its parent function on AE, so the hook installed but never ran and outfits stayed invisible. It now hooks the correct site (verified against 1.6.1170). Skyrim SE is unaffected.
- Fixed the inventory's floating item model showing through the editor on AE. The inventory loads that preview model asynchronously there, so the one-time clear on open could miss it; it is now cleared again as it appears.
- Helmet Toggle 2 compatibility: while Helmet Toggle keeps your worn headgear hidden, a styled helmet now hides with it instead of forcing itself visible, and returns when you show headgear again. Styles in other head slots (a circlet under a hidden hood) are unaffected. No effect without Helmet Toggle 2 installed.

## 0.1.0

Initial release. One build for Skyrim SE 1.5.97 and AE 1.6.1130+.

- ESO-style transmog: wear up to six saved outfits as a pure appearance layer over your real equipment. Stats, enchantments and weight never change.
- In-game editor with live preview, controller-first, drawn over the inventory through the FLICK UI framework.
- Render-layer override by construction: no inventory changes, no equipment scripts, no dynamic forms. Outfits persist in SKSE co-save data, per save, with a shared JSON library as the seed.
- Body-aware fit detection, auto-discovered presets from what you own (re-scanned each time you open the editor, so newly crafted or looted gear shows up), and a showcase preset library to import from.
- Optional Seamstone lore item (a small ESP): a carryable Alteration focus sold by Farengar in Dragonsreach that opens the editor in the world. Without it, a hotkey opens the editor.
- Optional gold cost for creating outfits, ESO-style (toggleable).
- Screen Archer Menu support: open the editor from SAM's menu while posing.
- Safety guards: every engine hook site is byte-checked before patching and fails safe; on AE the sites resolve through RELOCATION_ID (verified against 1.6.1170) and self-disable on a byte mismatch.
