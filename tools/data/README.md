# eso_dyes.tsv

Every dye in The Elder Scrolls Online, scraped from the UESP wiki's
`Online:Dyes` page on 2026-08-02. Reference data for curating Fitting Room's
palette. Nothing reads it at runtime; a converter turns the rows we choose into
a pack under `dist/SKSE/Plugins/FittingRoom/Dyes/`.

## Columns

| Column | Meaning |
|---|---|
| `name` | The dye's name in ESO |
| `hue` | ESO's own filter bucket: Red, Yellow, Green, Blue, Purple, Brown, Grey, Mixed, Iridescent |
| `rarity` | Common, Uncommon, Rare, Material, Dye Stamp |
| `hex` | RRGGBB, converted here from the HSL the wiki publishes |
| `eso_achievement` | The ESO achievement that unlocks it |

## Two things to know before using it

**The hex is derived, not published.** UESP gives HSL to one decimal place and
the conversion is ours, so a value can sit a bit or two off whatever ZeniMax
stores. That is well inside the range where a dye still reads as the same
colour, but it is not a byte-exact import and should not be described as one.

**`eso_achievement` names an ESO achievement, not a Skyrim one.** Most have no
Skyrim equivalent at all: there is no Alliance War, no Cyrodiil campaign, no
trial. The column is kept because a useful minority do map cleanly, Lycanthropy
and Vampirism and the crafting masteries among them, and because it records what
a colour was originally meant to say about the player wearing it. Anything
gating a colour in Fitting Room has to be evaluated against Skyrim state.

## Buckets worth separating

`Dye Stamp` rows are Crown Store items in ESO rather than achievement rewards,
and they are the loudest colours in the set. `Material` rows are the ten
crafting-tier metals and read as a family, not as choices.
