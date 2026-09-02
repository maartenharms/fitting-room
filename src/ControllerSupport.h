#pragma once

// ⚠⚠ CONTROLLER SUPPORT IS DEFERRED TO A FUTURE UPDATE AND IS SWITCHED OFF FOR
// THE 1.0.0 RELEASE. User's decision, 2026-08-15: "we are dropping controller
// support as it's not functional and i'm not shipping partial broken controller
// controls... we will defer proper controller support later in a future update".
//
// Four navigation models were built and field tested in one day and every one was
// rejected: patched ImGui nav, an editor-owned focus registry, a stick-driven
// mouse pointer, and that pointer with the game cursor forced off. What survived
// the revert was a handful of bindings, and the problem with shipping THOSE is
// that hiding the cheat sheet would not make them go away: a player who happens
// to press Y or a trigger would still get behaviour, undocumented, from a feature
// the mod does not claim to have.
//
// ⚠ SO THE SWITCH IS THE BINDINGS AND THE HINT LINE TOGETHER. Turning off only
// the hints would ship exactly the partial, undocumented controls this decision
// is about.
//
// ⚠ WHAT IS **NOT** BEHIND THIS SWITCH, AND WHY:
//
//   * The RIGHT STICK camera (turn and zoom the character). Asked for by name,
//     field confirmed working, and it is not navigation: nothing about it is
//     partial. See EditorUI's ApplySubjectSpin.
//   * LB / RB on the presets and styles panes. Those predate all of this work
//     (6d5ff25, OS-56) and have shipped before; they are not part of what is
//     being deferred.
//   * `##navsentinel` and `SetNavCursorVisible(false)` in EditorUI. Neither is a
//     control. The sentinel guards a CTD in FUCK's own ImGui at
//     `NavMoveRequestApplyResult`, which zero nav-eligible items makes reachable
//     on any d-pad press, so it must stay whatever the pad is allowed to do.
//
// ⚠ TO BRING IT BACK: flip kEnabled and the bindings and the hint line return
// together. That is the whole restore, and it is deliberately one line so the
// future update starts from a known state rather than from an archaeology
// exercise. The four rejected models are recorded in docs/STATUS.md under the
// 2026-08-15 controller entries; read those before designing a fifth.
namespace OS::ControllerSupport {

    inline constexpr bool kEnabled = false;

}  // namespace OS::ControllerSupport
