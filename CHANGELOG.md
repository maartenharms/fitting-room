# Changelog

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
