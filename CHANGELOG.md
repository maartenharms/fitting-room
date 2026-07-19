# Changelog

## 0.2.0 (2026-07-19)

**New**

- **Weapon and quiver transmog.** Outfits can now restyle your weapons the way they restyle your armor. You set a look per weapon class - sword, dagger, war axe, mace, greatsword, battleaxe and warhammer, bow, crossbow, staff, arrows and bolts - and every weapon of that class you draw wears it. It is an appearance layer like the rest of the mod: the real weapon keeps its damage, speed, enchantment, animations and name, nothing is added to or removed from your inventory, and unequipping is unaffected. Because the swap happens where the game loads the weapon's model, mods that read that model follow along: Simple Dual Sheath mirrors the styled scabbard, Immersive Equipment Displays keeps its placement, physics and enchantment glows attach to the styled mesh. A style must match the weapon's class (a sword can only look like another sword) so grips, sheathes and draw animations stay correct. Quivers are included, with one rough edge in this version: a new quiver look appears only after you unequip and re-equip your ammunition, because arrows attach to your character through a different path in the game than weapons do. Everything else applies immediately. Closing that gap is the first item for the next version.
- **Follower and NPC transmog.** You can now dress your followers. The editor has an "Editing:" picker at the top listing you and your current followers; choose one and everything works as it does for you - browse, preview live on them, Apply. Each follower keeps their own outfits. A look survives dismissing and re-recruiting them, and save/load. Followers you have dressed but who are not with you stay in the list marked "(away)" so you can see or remove their look. Changing a follower's outfit still costs your gold, if the gold option is on.
- Weapon looks can be shared in preset files: preset authors can add an optional `weapons` list beside `slots`. See PRESETS.md for the format.

**Fixed**

- **Camera control with Screen Archer Menu.** With the editor open over SAM you can now frame a shot using SAM's own controls: right mouse to pan, left mouse to pivot around your character, wheel to zoom. Two separate things were blocking it. The mod was driving the camera itself on the same drag SAM orbits with, so the two fought over it; when the editor is opened from SAM the camera is now left entirely to SAM. Separately, the editor only passed your mouse through to the game once a button was already held down, which meant the button press that begins a drag was the one event that never got through, so SAM never saw a drag start. That is why the camera would only half respond, and only while two buttons were held at once. The editor now also opens on the same side of the screen as SAM's own panel rather than the opposite side, so the rest of the view is free to drag in instead of a narrow strip between the two. Without SAM, the free-camera drag added in 0.1.2 is unchanged; that entry's note about it working with SAM's free camera is superseded by this.

**Changed**

- The quiver slot is now labelled "Quiver" rather than "Arrows", and the bolt slot "Bolt Quiver". What you restyle there is the quiver on your back, not the ammunition inside it, and the old wording read as though it would change your arrows.
- The outfit tabs are evenly sized now. Each one used to stretch to fit its own name, so a library with a long name and a short one beside it looked ragged.
- The editor and settings panels line up properly. Every tick box sat on the wrong side of its row and never matched the sliders above and below it, because the mod was overriding FLICK's own layout instead of leaving it alone. Thanks to Fuzzles for catching it.
- "On the Outward Art" is no longer added to your inventory, and any copy you were handed is taken back. The book does nothing yet, so it was only ever clutter; it comes back when reading it actually does something. The Seamstone is unaffected.
- On a follower, a styled slot only shows where they are actually wearing something. This is deliberate: it keeps their look in step with mods that equip and unequip their gear (Helmet Toggle 2, AI changing armor) instead of fighting them. Your own character is unchanged - your outfit applies as it always has.
- Transmog now stands down while a follower (or you) is transformed into a werewolf or a Vampire Lord, and comes back on returning to normal. Ordinary vampirism is unaffected - your look stays on.

**Compatibility**

- Existing saves and outfit libraries load unchanged. Outfits saved by this version can carry weapon looks; an older build reading them simply ignores the weapon part.

**Not yet supported**

- Hiding a weapon (as opposed to restyling it) is not available. The game itself controls whether a weapon is drawn or sheathed and rewrites that state continuously, so a "hide" that worked at first would silently stop the moment you drew your weapon; it needs a different approach and is planned for a later version. Shields are also not styleable yet.

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
