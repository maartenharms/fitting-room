#pragma once

// One lighting property and its material written as one line: the material's
// identity, every texture slot its class holds, the property's emissive state
// and the shader flags that decide whether any of it renders.
//
// ⚠⚠ SHARED SO THAT BEFORE AND AFTER ARE THE SAME SENTENCE. The eye dye reads
// it either side of its own swap, and the whole value of the second reading is
// that a slot which was populated and is now empty stands out by eye. Two
// divergent log formats would have to be compared field by field, which is
// where the OS-192 chain was nearly lost.
//
// ⚠ THE MATERIAL POINTER IS PRINTED, NOT ONLY ITS CONTENTS, and that is
// deliberate: a swap and a rebuild look identical in the values alone.
// compare-identity-not-value-to-separate-a-write-from-a-rebuild.
//
// ⚠⚠ STRICTLY READ-ONLY. Every material this reaches may be the POOLED
// instance that other actors are pointing at, and OS-192 was a CTD bought by
// calling ClearTextures() plus OnLoadTextureSet on exactly these eye materials.
// Nothing here writes anything. See shader-materials-are-pooled-never-write-one.
//
// ⚠ MAIN THREAD. A lighting property may be rebuilt underneath a Present-thread
// reader.
//
// This was the shared half of src/HeadCensus.*, the dyeing-eyes instrument
// deleted at the 1.0.0 release. The census was gated on [Debug] bHeadCensus and
// went with the key; this never was, because the dye's own log lines are not an
// instrument that can be switched off.

#include <string>

namespace RE {
    class BSLightingShaderProperty;
}

namespace OS::MaterialReading {

    [[nodiscard]] std::string Describe(RE::BSLightingShaderProperty* a_prop);

}  // namespace OS::MaterialReading
