#pragma once
#include "PCH.h"

// A MenuEventHandler this plugin can implement on alandtse's CommonLibSSE-NG.
//
// The library declares only the destructor and CanProcess as virtual on
// RE::MenuEventHandler. The four Process* methods are non-virtual wrappers
// that dispatch through the game's own vtable, because AE 1.7.99 inserted two
// entries (a motion gesture and a sixaxis handler) ahead of them, so no one
// C++ layout is right for every runtime. A subclass that redeclared them
// virtual would be laid out for one game and called through the other's.
//
// So the layout is built here. Impl declares the four in the order the 1.5.97
// and 1.6 games dispatch (slots 2 to 5, straight after CanProcess), and
// Install rebuilds the object's vtable for 1.7.99 and later: the same six
// entries with two declined entries ahead of the four handlers. Derive from
// Impl, override what you need, and call Install once before
// MenuControls::AddHandler; log what it returns beside the registration, so a
// tester's log says which layout the handler was given.
namespace OS::MenuHandlerVtable {
    struct Impl : RE::MenuEventHandler {
        // ⚠ DECLARED IN THE 1.5.97 AND 1.6 SLOT ORDER. Do not reorder.
        virtual bool ProcessKinect(RE::KinectEvent*) { return false; }          // 02
        virtual bool ProcessThumbstick(RE::ThumbstickEvent*) { return false; }  // 03
        virtual bool ProcessMouseMove(RE::MouseMoveEvent*) { return false; }    // 04
        virtual bool ProcessButton(RE::ButtonEvent*) { return false; }          // 05
    };

    // Give a_handler the vtable the running game dispatches through. Safe to
    // call again on the same object. Returns the layout used, for the log.
    const char* Install(Impl* a_handler);
}
