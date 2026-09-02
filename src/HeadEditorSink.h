#pragma once

// Head-editor coexistence sink.
//
// The vanilla character editor ("RaceSex Menu" - RaceMenu reskins it rather
// than replacing it, so this covers both) rebuilds the player's head on the way
// in. That rebuild runs the engine's own hair painter,
// BSFaceGenManager::PrepareHeadPartForShaders (SE 26259 / AE 26838), which
// derives the tint from the actor base and writes it over every hair-tint
// material - painting straight over whatever HairColor::Repaint had put on the
// geometry. Nothing tells Fitting Room this happened: in the 2026-07-30 field
// log the plugin simply stops logging at the moment the menu opens.
//
// This is not a conflict of method. RaceMenu's own RGB slider drives the same
// engine painter Fitting Room does, and skee64 exposes no hair-tint interface
// to integrate with - its published IPluginInterface.h declares ten interfaces
// and the only colour methods on any of them are item TEXTURE LAYER colours,
// i.e. worn gear. There is no API here to defer to or hook. The only thing that
// decides the outcome is who writes last, so Fitting Room writes again on the
// way out.
namespace OS::HeadEditorSink {

    // Register the MenuOpenCloseEvent sink against RE::UI. Call once at
    // kDataLoaded, alongside the other UI sinks.
    void Register();

    // Drop the open-edge reading of a character-editor visit.
    //
    // ⚠ WIRED INTO Persistence::RevertCallback beside HairColor::Clear and
    // HeadPart::Clear, and for the same reason they are: the reading describes
    // the character being torn down. Unlike those two it authorises DELETING
    // things the player paid for, so a reading that survived a load would grade
    // the next character's editor visit against the previous one's face.
    void Forget();

}  // namespace OS::HeadEditorSink
