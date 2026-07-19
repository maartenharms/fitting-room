# Showcase presets: ship your armor's intended look

Fitting Room can show players your armor set the way you meant it to be worn.
You ship one small JSON file inside your mod; Fitting Room finds it, lists it
under a **Showcases** tab in its outfit editor, previews it live on the
player's own character, and lets them copy it into their outfit library with
one click. No scripting, no patch, no dependency on you doing anything else.

A showcase is pure data. It never touches the player's inventory, never
equips anything, and works even if the player has never found a single piece
of your set; the preview is the advertisement.

## What players see

- A **Showcases** tab appears in the Fitting Room editor whenever at least
  one preset is installed. Presets are grouped by author.
- Clicking your preset previews it instantly on their character, using the
  same render override the rest of the mod uses. Previewing is free and
  changes nothing until they act.
- **Save to my outfits** copies it into their library (they own the copy and
  can edit it; the file is never modified).
- A piece that comes from a plugin the player does not have is shown dimmed
  as missing; the rest of the fit still previews. See `requires` below for
  when you want the whole preset to hide instead.

## The five-minute workflow

You do not need to write JSON by hand. Fitting Room writes it for you.

1. In game, open the Fitting Room editor and build the fit: pick your pieces
   per slot, hide the slots that should be bare. Name the outfit what the
   preset should be called ("Ebony Vanguard, Full Plate").
2. Click **Export** (next to the outfit name). This writes
   `Data/SKSE/Plugins/FittingRoom/Exports/<name>.json` in exactly the format
   below, with `requires` prefilled with every non-vanilla plugin the fit
   references.
3. Open the file, fill in `author` and `description`, and trim `requires`
   down to the plugins that are genuinely mandatory (usually just your own;
   see semantics below).
4. Rename the file so it cannot collide with anyone else's:
   `<YourModName> - <PresetName>.json`. **This matters:** mod managers merge
   every mod's Presets folder into one virtual folder, and two files with the
   same name silently become one. Unique filenames are load-bearing.
5. Ship it in your mod archive at:

   ```
   SKSE/Plugins/FittingRoom/Presets/<YourModName> - <PresetName>.json
   ```

6. Verify: launch the game, open the editor, Showcases tab, click **Rescan**.
   Your preset appears, previews, and logs one confirmation line (see
   Troubleshooting). You can iterate without relaunching: edit the file, click
   Rescan again.

## File format

The shipped sample (`Presets/OutfitSlots - Nightingale Sample.json`) is a
complete working preset:

```json
{
  "version": 1,
  "name": "Nightingale, Unhooded",
  "author": "Fitting Room",
  "description": "Full Nightingale leathers with the hood down.",
  "requires": ["Skyrim.esm"],
  "slots": [
    { "slot": 31, "kind": "hide" },
    { "slot": 32, "kind": "style", "mod": "Skyrim.esm", "id": "0x0FCC0F" },
    { "slot": 33, "kind": "style", "mod": "Skyrim.esm", "id": "0x0FCC11" },
    { "slot": 37, "kind": "style", "mod": "Skyrim.esm", "id": "0x0FCC0D" }
  ],
  "weapons": [
    { "class": "sword", "kind": "style", "mod": "Skyrim.esm", "id": "0x013989" },
    { "class": "bow",   "kind": "style", "mod": "Skyrim.esm", "id": "0x0139C0" }
  ]
}
```

| Key | Required | Meaning |
|---|---|---|
| `version` | yes | Schema version. Always `1` for now; a file with a higher version is skipped with a log line, so older Fitting Room builds never misread newer presets. `weapons` was added without a version bump, so it is optional and older builds simply ignore it. |
| `name` | yes | Shown in the Showcases list, and becomes the outfit's name when a player saves it. |
| `author` | no | Your handle. Presets are grouped by this in the browser. |
| `description` | no | One or two sentences shown in the detail pane. Searchable. |
| `requires` | no | Array of plugin filenames. If any is missing from the load order, the whole preset is hidden (one log line says why). |
| `slots` | one of | The armor fit. At least one usable `slots` OR `weapons` entry is required. |
| `weapons` | one of | The weapon looks (optional). Same requirement: a preset needs at least one usable entry across `slots` and `weapons` combined. |

Each `slots` entry:

| Key | Meaning |
|---|---|
| `slot` | Biped slot number, 30 to 61 (the numbers you know from xEdit and mod pages). Slot 39 (shield) is ignored by design. Out-of-range entries are skipped. |
| `kind` | `"style"` (show this armor's look here) or `"hide"` (render the slot bare; body slots show skin, helmets show hair). |
| `mod` | For `"style"`: the plugin that DEFINES the armor record. Use the plugin that adds the record, not a patch that overrides it. |
| `id` | For `"style"`: the record's LOCAL FormID as hex. Strip the load-order prefix (the first two hex digits): xEdit's `2A000D62` becomes `"0x000D62"`. For ESL/ESL-flagged plugins (`FE xxx` prefix) keep only the last three hex digits: `FE01D803` becomes `"0x000803"`. The Export button computes this correctly for every case; hand-writing it is where mistakes happen. |

Each `weapons` entry styles a whole weapon class ("every sword you draw looks
like this"). The real weapon keeps its stats, enchantment and animations; only
the model changes.

| Key | Meaning |
|---|---|
| `class` | One of a fixed set of 11: `sword`, `dagger`, `waraxe`, `mace`, `greatsword`, `battleaxe`, `bow`, `crossbow`, `staff`, `arrows`, `bolts`. (`battleaxe` covers both battleaxes and warhammers; `greatsword` is two-handed swords. `arrows`/`bolts` style the quiver.) An unknown class name is ignored. |
| `kind` | `"style"`. Hiding a weapon is not supported in this version, so a `"hide"` weapon entry is inert (the real weapon still renders). |
| `mod` | The plugin that DEFINES the WEAP or AMMO record (for `arrows`/`bolts`, an AMMO record). |
| `id` | The record's LOCAL FormID as hex, the same rule as `slots`. Match the class to the record: a `sword` class must reference a one-handed sword WEAP, `arrows` an arrow AMMO, and so on. A mismatched record is skipped. |

Formatting rules: the file must be valid JSON (no comments, no trailing
commas) in UTF-8. Unknown keys are ignored, so you may keep private metadata
in your own keys. Files over 256 KB are skipped as a safety valve.

## Semantics worth knowing

**`requires` is a hard gate; absent style plugins are a soft one.** If a
plugin listed in `requires` is missing, your preset is skipped entirely.
If a `slots` entry references a plugin that is missing but NOT listed in
`requires`, only that entry goes inert (shown as missing in the detail pane,
skipped by the renderer). Use this deliberately:

- Your own plugin belongs in `requires`; a showcase of gear the player
  cannot render is noise.
- Optional integrations do not. A cape slot referencing another author's
  cloak mod, left out of `requires`, means players with the cloak mod see the
  full fit and players without it see everything else.

**Slots are engine slots.** A multi-slot armor (a robe covering 32+34+38, for
example) is one entry at the slot where you would expect to find it; Outfit
Slots renders the item's full coverage, the same way the engine does. You
cannot split one item across styles.

**The player's body wins.** A preset references your ARMO records; meshes
resolve through the player's own load order. Body-mod refits, texture
replacers and so on apply exactly as they do when the item is worn normally.

**Players keep a copy, not a link.** Saving a showcase copies it into the
player's library (capped at 10 outfits). Editing or removing your preset
later never breaks a saved copy; a missing plugin only stops the affected
pieces from rendering.

## Troubleshooting

Everything the loader decides is one line in
`Documents/My Games/Skyrim Special Edition/SKSE/FittingRoom.log`, prefixed
`PresetStore:`.

| Log line | Meaning |
|---|---|
| `PresetStore: 'Name' by Author (file.json).` | Loaded; it is in the browser. |
| `SKIP 'file.json': not valid JSON (...)` | Syntax error; the parser's message includes the line. Trailing commas and comments are the usual suspects. |
| `SKIP 'file.json': "name" is required` | Metadata problem; the message names the key. |
| `SKIP 'file.json': version 2 (...)` | The preset targets a newer Fitting Room. |
| `SKIP 'file.json': requires 'X.esp' (not in the load order)` | The hard gate did its job. If that surprises you, trim `requires`. |
| `SKIP 'file.json': no usable "slots" or "weapons" entries (...)` | Every entry was out of range or malformed. Check the slot numbers and weapon class names. |
| Nothing mentions your file | The file is not in `Data/SKSE/Plugins/FittingRoom/Presets/` after VFS merging, or it does not end in `.json`. Check your archive's folder layout against step 5 above. |

A preset that loads but previews with a missing piece logs nothing; the
detail pane shows `<missing plugin: X.esp>` on the affected slot instead.

## Stability promise

The schema is versioned and version 1 is frozen: future Fitting Room
releases will keep reading this exact format. If the format ever grows, new
keys will be optional and old files will keep working; a breaking change
would mean `version: 2` files, and version-1 files would STILL keep working.
Ship your preset once and forget about it.
