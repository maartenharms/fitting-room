Scriptname OutfitSlotsSAM Hidden
{Screen Archer Menu (and any menu) integration for Outfit Slots.
 OpenEditor is a native global implemented in OutfitSlots.dll - it opens the
 Outfit Slots editor. Wire a SAM menu entry to it with:
     global:
       script: OutfitSlotsSAM
       func: OpenEditor}

; Native global - no body. Implemented in OutfitSlots.dll (SamCompat.cpp).
Function OpenEditor() global native
