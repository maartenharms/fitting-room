#pragma once

namespace RE {
    class Actor;
}

namespace OS::MenuStudioApi {

    // Thin client for Menu Studio's public C ABI (MenuStudio_* exports, API
    // version 1). Nothing is linked: the DLL is resolved at runtime with
    // GetModuleHandleW + GetProcAddress, the same way extern/FUCK_API.h reaches
    // FUCK.dll, so a load order without Menu Studio simply never resolves and
    // every call below is a no-op.
    //
    // WHY THIS EXISTS. Menu Studio hides every nearby actor while its bubble is
    // armed, exempting only the player and their mount, across four separate
    // cull paths. Measured on 2026-07-31 (OS-97): editing Jenassa with the
    // bubble up, her 3D read appCulled=yes while the player read no, in the same
    // frame. Without this call the editor cannot show the follower it is
    // dressing, no matter what the camera does.
    //
    // ⚠ MAIN THREAD ONLY. The exports touch form lookup and declutter state.

    // Is Menu Studio present and answering at a version we understand?
    [[nodiscard]] bool Available();

    // Ask Menu Studio to keep this actor in the shot. Null clears.
    //
    // Returns false if Menu Studio is absent, or if it refused the actor -
    // which is what an unloaded or "(away)" follower gets, and is the honest
    // answer rather than a handle quietly retained against a character who is
    // not there.
    bool SetFramedCompanion(RE::Actor* a_actor);

    // The same thing, queued onto the main thread, for callers that live on
    // the render thread. The editor's target switch and its open/close run
    // inside FLICK's Present hook, and the exports touch form lookup and
    // declutter state, so calling straight through from there would be a race.
    // An empty handle clears, and a handle that no longer resolves also clears,
    // which is the right answer for a follower who unloaded between the switch
    // and the task running.
    void SetFramedCompanionDeferred(RE::ActorHandle a_actor);

    // Has a declutter sweep run since the last successful set? False right
    // after setting and true once the shot has caught up. Only meaningful for
    // reporting: the sweep runs on its own schedule and nothing here waits.
    [[nodiscard]] bool TookEffect();

    // Would Menu Studio accept this actor as the framed companion right now?
    // Used to mark a follower in the roster BEFORE the user picks her, since
    // picking one Menu Studio will not frame gets a shot that quietly falls
    // back to lighting the player instead.
    //
    // ⚠ TRUE WHEN THE ANSWER IS UNKNOWN. Menu Studio absent, too old to export
    // this, or refusing to answer all mean the editor has no framing to lose,
    // so every target stays selectable and the roster behaves exactly as it did
    // before this existed. A "cannot be framed" mark is only ever shown on
    // Menu Studio's own authority.
    [[nodiscard]] bool CanBeFramed(RE::Actor* a_actor);

    // ---- the shot, Menu Studio API 2 ------------------------------------
    // Point Menu Studio's camera at the part of the character being edited: a
    // helmet wants the head framed, boots want the feet, and neither Menu
    // Studio nor a fixed pivot setting can know which.
    //
    // a_nodeName is a skeleton node ("NPC Head [Head]"); null or empty clears.
    // a_closeness runs 0 for as far out as the boundary allows to 1 for as
    // close as it allows, and NEGATIVE moves the pivot only and leaves the
    // distance alone.
    //
    // ⚠ THE PIVOT STICKS AND THE DISTANCE DOES NOT, by Menu Studio's design.
    // Keeping the pivot is the point, since the player goes on orbiting the
    // head for as long as they are editing it; keeping the distance would
    // fight them the moment they touched the wheel. So closeness is a request
    // made once and then owned by ordinary zooming, which is why this is
    // called on a CHANGE of what is selected and not every frame - a per-frame
    // call would re-take the distance the player just set.
    //
    // ⚠ MAIN THREAD ONLY, and the editor draws on the render thread. Every
    // caller in this codebase is inside FLICK's Present hook, so route through
    // a task the way SetFramedCompanionDeferred does.
    //
    // Silent no-ops when Menu Studio is absent, predates API 2, or has its
    // camera switched off. That last one is Menu Studio's consent gate and not
    // ours to work around.
    void FocusShotOnNode(const char* a_nodeName, float a_closeness);

    // The same ask for something WORN. Menu Studio measures the geometry
    // hanging off the node and takes both the pivot and the distance from it,
    // so a dagger arrives tight and a greatsword arrives wide without this
    // side knowing how long either is.
    //
    // a_fallbackCloseness is used only when Menu Studio finds nothing to
    // measure there, where this is exactly FocusShotOnNode.
    //
    // ⚠ RESOLVED BY NAME AND NOT BY VERSION, because it was added without a
    // bump. A Menu Studio reporting API 2 may or may not export it, which is
    // the same shape ShotWasPanned already has.
    void FocusShotOnAttachment(const char* a_nodeName, float a_fallbackCloseness);
    void ClearShotFocus();

    // Has the player moved this framing by hand, with a middle-drag pan, since
    // the last focus change or recentre?
    //
    // Asked on a PAGE CHANGE, which is the one moment the editor has to decide
    // whether a shot is still wanted. A player who nudged the framing onto a
    // buckle wants it kept while they go and pick a colour; one who touched
    // nothing wants the whole character back. Menu Studio answers false when
    // its camera is not armed or the player has it switched off, which is the
    // honest answer to "did the player frame this" in both cases.
    //
    // ⚠ MAIN THREAD ONLY, LIKE EVERY EXPORT HERE, AND THAT SHAPES THE CALLER.
    // This is a QUESTION rather than an instruction, so a caller on the render
    // thread cannot marshal it and then branch on the answer in the same frame.
    // The editor asks and acts inside one task; see RequestPageChangeShot.
    //
    // ⚠ TRUE WHEN THE ANSWER IS UNKNOWN, and this is a decision rather than a
    // fallback that fell out. Menu Studio bumps its API only for a BREAKING
    // change, so a build can export the shot pair and not this one; no PUBLIC
    // build does (0.7.2 has neither export, and every build with the pan has
    // this), which leaves the branch reachable only from a dev Menu Studio. It
    // is resolved the user's way for when it is reachable: a framing kept when
    // it should have been dropped costs one middle click, and a framing dropped
    // when it should have been kept costs the player the work they just did.
    [[nodiscard]] bool ShotWasPanned();

    // ---- Menu Studio's appearance button ---------------------------------
    // Menu Studio registers 'Change your appearance' on its strip BORN
    // HIDDEN: the author's call is that face sculpting belongs to the
    // styling context, and the styling context is this editor's WINDOW - a
    // context finer than any menu name, whose open and close only this side
    // sees. EditorWindow::SetOpen calls this on both edges, so the button
    // exists exactly while the editor does.
    //
    // ⚠ MAIN THREAD ONLY, like every export here. SetOpen already runs
    // there (Toggle/RequestOpen/RequestClose marshal through the task
    // interface), so the edges need no second marshal.
    //
    // Silent no-op when Menu Studio is absent or predates the export: the
    // strip is Menu Studio's, so with it gone or old there is nothing to
    // show or hide and nothing is lost by saying nothing.
    void SetAppearanceButtonVisible(bool a_visible);

    // Show or hide ANY action-bar button by its id, including one this plugin
    // registered itself. Same export, same silent-when-absent posture; the
    // helper above is this with Menu Studio's own id baked in.
    void SetActionVisible(const char* a_id, bool a_visible);

    // Fill a button's background to show how full a resource is, 0 to 1.
    // NEGATIVE clears the meter, which is deliberately not the same as 0: zero
    // draws an empty bar, which is what an empty Seamstone should look like,
    // while negative puts the tile back to plain.
    //
    // ⚠ THE POINT IS TO ANSWER BEFORE THE CLICK. Under iCostMode=2 an empty
    // stone blocks styling outright, and the strip is where a player looks
    // first. A tile that shows its own charge turns "press it and find out"
    // into something readable at a glance.
    //
    // False when Menu Studio is absent, predates the export, or has no button
    // under that id yet. Silent either way: the tile simply draws plain, which
    // is exactly what it did before this existed.
    bool SetActionMeter(const char* a_id, float a_fraction);

    // Leave a button on the strip but refuse it, with a_reason shown on hover
    // in place of its label.
    //
    // ⚠ THE RIGHT ANSWER FOR A PRECONDITION, WHERE HIDING IS THE WRONG ONE.
    // Hiding says there is nothing here; this says there is something and you
    // cannot have it yet, which is what the character editor's door fee needs.
    // A player who cannot pay should see the door and be told, not watch a
    // button vanish from under them (user 2026-08-07).
    //
    // Silent no-op on a Menu Studio without the export, which means the button
    // stays fully live there. That is the honest fallback: an older strip
    // cannot refuse anything, and pretending otherwise by hiding it would swap
    // one wrong answer for another.
    void SetActionEnabled(const char* a_id, bool a_enabled, const char* a_reason);

    // ---- the owner context -----------------------------------------------
    // Tell Menu Studio that this editor owns the screen. With Menu Studio's
    // bWaitForOwnerContext setting on, a menu is still counted at open but the
    // studio session stays down: no pause, no studio camera, no lights, no
    // backdrop, no declutter, until an owner publishes a live context. This
    // call is that publication, and withdrawing it stands the studio back down
    // while the inventory underneath stays open.
    //
    // Menu Studio holds a SET of live owner ids rather than a flag named after
    // this mod, so the id is baked in here and no caller can spell it two ways.
    // Both edges must pass the same string or the close never withdraws what
    // the open published.
    //
    // ⚠ MAIN THREAD ONLY, like every export here. EditorWindow::SetOpen already
    // runs there, so its two edges need no second marshal.
    //
    // ⚠ RESOLVED BY NAME AND NOT BY VERSION. It joined the API without a bump,
    // the same way FocusShotOnAttachment and ShotWasPanned did, so a Menu Studio
    // reporting API 2 may or may not export it. Absent resolves to nothing
    // happening, which is the correct answer rather than a fallback: a build
    // that cannot hear this also cannot hold its bubble down waiting for it.
    //
    // ⚠ A CONTEXT DOES NOT OUTLIVE THE MENU IT WAS OPENED IN. Menu Studio
    // clears the set when the last covered menu closes, so that an owner who
    // misses its own close edge cannot pin the studio open for the session.
    // This side publishes again on every open rather than assuming the last one
    // still stands.
    void SetOwnerContext(bool a_active);

    // Ask Menu Studio's owner-scoped ledger to keep the floating inventory item
    // preview hidden. Returns true only when the optional export exists and
    // accepts this mod's claim. The caller records that answer and releases
    // exactly what it acquired; Fitting Room's local hide paths remain the
    // fallback when Menu Studio is absent or older.
    //
    // Main thread only, like SetOwnerContext. Resolved by export name rather
    // than API version because this is an additive entry point.
    [[nodiscard]] bool SetItemPreviewSuppressed(bool a_suppressed);

}  // namespace OS::MenuStudioApi
