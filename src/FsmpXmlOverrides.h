#pragma once

// r53: the look's wig xml (GuanYinping.xml) registers the whole breast and
// butt family as plain <bone> collision anchors, and FSMP re-poses
// registered bones from the animation AFTER CBPC adds its offsets - CBPC's
// own log computes breast Diffs of 0.4 while the screen stays still. The
// Umbrael save's hair registers no body bones, which is the whole
// asymmetry the physics hunt chased across seven rounds.
//
// The authored map (dist\...\fsmp\fsmp-xml-overrides.json) names wig xmls
// that hold body bones and the stripped copies to use instead
// (tools/fsmp_xml_strip.py). At wig attach, the fresh root's
// 'HDT Skinned Mesh Physics Object' extra data is re-pointed at the
// stripped copy BEFORE the FSMP publish reads it. Cost: that wig stops
// colliding with the chest; the body keeps its physics.

namespace RE {
    class NiAVObject;
}

namespace OS::FsmpXmlOverrides {

    // kDataLoaded: read the map. Missing file = empty map = every swap a
    // no-op; logged once either way.
    void Load();

    // If a_root (a wig root FR just claimed) carries a physics xml the map
    // names, point the extra data at the replacement. Returns true when a
    // swap happened. Game thread, before the publish.
    bool SwapIfMapped(RE::NiAVObject* a_root);

}  // namespace OS::FsmpXmlOverrides
