"""Append translation keys to a UTF-16LE CRLF translations file. Run from the
worktree root.

⚠ KEYS HOLDS THIS STINT'S BATCH AND NOTHING ELSE. Every previous batch is
already in the file, and leaving it here is not free: a value that has since
been corrected in the file, by a tooltip pass or a reword, would be sitting in
this list in its OLD form, waiting to be written back over the fix.

⚠ AND THAT IS WHY AN UPDATE HAS TO BE ASKED FOR. A two-item entry adds a key
and never touches one that exists. A three-item entry ends in True and means
"this key's English has been rewritten, put the new text in". An add-only entry
therefore cannot clobber anything, which is what makes a stale line here
harmless rather than destructive.

That is not hypothetical. Updating on difference alone silently put the trailing
full stops back on four Shape tooltips and reverted a fifth string, because this
list still carried the OS-161 batch as it was written before the "no tooltip
ends in a period" pass corrected it in the file (2026-08-08).
"""
import io
import sys

KEYS = [
    # ---- the preview and the worn rule (user "do C", 2026-09-02) ----
    # In lore friendly the render gate's worn rule stands down for the actor
    # being staged while the editor is open, so a preset draws whole on a bare
    # character in here and loses those pieces the moment the editor shuts.
    # These say so before it happens: the Presets status line with the count,
    # the slot row's tag, and the row's tooltip.
    ("$FR_TryingOnBare",
     "Trying on '%s'. Nothing is worn under %u of its %u pieces, so outside "
     "the fitting room the lore rule hides them"),
    ("$FR_RowPreviewOnly", "[fitting room only]"),
    ("$FR_RowPreviewOnlyTip",
     "Nothing you are wearing sits under this piece, so it shows in the "
     "fitting room only. Outside, the lore rule hides it. Equip gear on a "
     "slot this piece covers, or turn off \"Transmog needs real gear "
     "underneath\" in Settings"),
]


def main(path):
    with io.open(path, "r", encoding="utf-16", newline="") as f:
        text = f.read()
    lines = text.split("\r\n")
    index = {}
    for i, line in enumerate(lines):
        if "\t" in line:
            index[line.split("\t", 1)[0]] = i

    added, changed, kept = [], [], []
    for entry in KEYS:
        key, value = entry[0], entry[1]
        may_update = len(entry) > 2 and entry[2]
        row = key + "\t" + value
        if key in index:
            if lines[index[key]] == row:
                continue
            if not may_update:
                # Present with different text and this entry did not ask to
                # change it. The FILE wins, and it says so out loud, because
                # silence here is how a corrected string gets reverted.
                kept.append(key)
                continue
            lines[index[key]] = row
            changed.append(key)
            continue
        while lines and lines[-1] == "":
            lines.pop()
        lines.append(row)
        added.append(key)

    text = "\r\n".join(lines)
    if not text.endswith("\r\n"):
        text += "\r\n"
    with io.open(path, "w", encoding="utf-16", newline="") as f:
        f.write(text)
    print("added   %d: %s" % (len(added), ", ".join(added) or "none"))
    print("updated %d: %s" % (len(changed), ", ".join(changed) or "none"))
    if kept:
        print("LEFT ALONE (file differs, entry did not ask to update): %s"
              % ", ".join(kept))
    total = len([l for l in text.split("\r\n") if "\t" in l])
    print("total keys now: %d" % total)


if __name__ == "__main__":
    main(sys.argv[1])
