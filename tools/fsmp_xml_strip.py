#!/usr/bin/env python3
"""Strip body-physics bones out of a hair SMP xml.

r53: the look's wig (GuanYinping.xml) registers the whole breast family as
plain <bone> collision anchors. FSMP re-poses registered bones from the
animation every frame, AFTER CBPC has added its offsets, so the body's
breast physics reads as computed in CBPC's log and static on screen. The
Umbrael save's hair (Kaysa.xml) registers no body bones, which is why she
jiggles and the switched character does not.

This writes a copy with every body-family <bone> line removed, verifies the
result still parses as XML, and refuses to write anything it cannot verify.
The runtime swaps the wig's 'HDT Skinned Mesh Physics Object' extra data to
the stripped copy per the authored map in fsmp-xml-overrides.json. Cost:
hair no longer collides with the chest; the body keeps its physics.

Usage: python fsmp_xml_strip.py <source.xml> <dest.xml>
"""

import re
import sys
import xml.etree.ElementTree as ET

BODY = re.compile(r"Breast|Butt|Belly|Pussy|Vagina|Genital|Clitoral", re.I)
BONE = re.compile(r"<bone\s+name=\"([^\"]+)\"\s*/>")


def main() -> int:
    src, dst = sys.argv[1], sys.argv[2]
    kept, dropped = [], []
    with open(src, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = BONE.search(line)
            if m and BODY.search(m.group(1)):
                dropped.append(m.group(1))
                continue
            kept.append(line)
    body = "".join(kept)
    # A dropped bone that anything else still references would leave a
    # dangling name; refuse rather than ship a file FSMP half-parses.
    for name in dropped:
        if name in body:
            print(f"REFUSED: '{name}' is still referenced after the strip")
            return 1
    ET.fromstring(body)  # raises if the result is not well-formed
    with open(dst, "w", encoding="utf-8", newline="\n") as f:
        f.write(body)
    print(f"{len(dropped)} body bone(s) stripped: {', '.join(dropped)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
