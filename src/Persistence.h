#pragma once

#include <cstdint>

namespace OS::Persistence {
    // The largest payload one co-save record may carry. Save and load must
    // agree on it: WriteRecord refuses to write past it and LoadCallback
    // refuses to read past it.
    //
    // Public rather than private to Persistence.cpp because a record's ENCODER
    // is the only place that can prove it never exceeds this, and that proof
    // wants to sit beside the caps it constrains. DyeUnlocks.cpp static_asserts
    // its own caps against this value; nothing else has needed to yet.
    //
    // ⚠ This guards EVERY co-save record. Widening it to make one record's
    // worst case fit weakens the guard on the outfit library, the appearance
    // collection, the NPC assignments and the two baselines at the same time.
    // Narrow the offending record's own caps instead.
    inline constexpr std::uint32_t kMaxRecordBytes = 1u << 20;

    // Call once from SKSEPluginLoad, before kDataLoaded.
    void Register();

    // Collections model: each SAVE owns its outfit library (written into the co-save
    // 'LIBR' record), so edits/deletes made on another save can't make outfits vanish.
    // Data/SKSE/Plugins/FittingRoom/outfits.json is the shared/global library - the
    // default a fresh game starts from and the fallback for pre-collections saves that
    // have no 'LIBR' record yet (they gain one the next time they are saved).
    //
    // Rules (Task 10) follow the SAME model, in their own 'RULE' co-save record, with
    // one deliberate difference: a save with no 'RULE' record gets an EMPTY rule set,
    // never the rules.json fallback outfits.json gives pre-collections saves above.
    // Generic outfit names recur across characters, so silently importing another
    // character's rules would make them fire on a character whose owner never wrote
    // them - see RuleStore.h and RuleStore::OnSaveLoaded. QueueLibrarySave below and
    // RuleStore::QueueMirrorSave call each other so outfits.json and rules.json are
    // always rewritten from the SAME save, never a mismatched pair.

    // Load the global library into the session. Call at kDataLoaded, before
    // any save loads.
    void LoadLibraryFileAtStartup();

    // Debounced, main-thread-marshaled save of the global library. Safe to
    // call from any thread; OutfitSession invokes it after every mutation.
    //
    // a_pairRules is an internal knob, not for external callers: it is how
    // RuleStore::QueueMirrorSave's own paired call INTO this function
    // (Task 10's mirror pairing) passes false, so the pairing is
    // STRUCTURALLY one hop deep - a call made with a_pairRules=false can
    // never itself trigger another pairing call, no matter how either
    // function's internal branches (the null-task-interface fallback, in
    // particular) get reordered later. Relying on the debounce flags alone
    // to stop that recursion was fragile: hoisting the task-interface check
    // above the pairing call - a natural-looking cleanup - clears the flag
    // before recursing and turns this into unbounded recursion.
    void QueueLibrarySave(bool a_pairRules = true);

    // Reconcile this character with the account-wide dye unlock file, BOTH
    // WAYS: take the colours other characters have earned, then publish the
    // ones this character holds. a_reason names the caller in the log.
    //
    // ⚠⚠ IT LIVES HERE BECAUSE SharedDyeUnlocks.cpp CANNOT HAVE IT.
    // SharedDyeUnlocksTests compiles that file directly with no engine, so a
    // Settings.h include there would break the suite. Same rule BodyMorphData
    // follows for the same reason. This file is engine-only and already owns
    // the save-side half of the same policy.
    //
    // ⚠ CALL IT WHENEVER THE ANSWER COULD HAVE CHANGED, not only at load. The
    // read half used to run at kNewGame/kPostLoadGame alone, so turning the
    // setting on mid-session did nothing until the next load and said nothing
    // about it either, which is what a field test reported as "sharing does not
    // work" (user 2026-08-11).
    //
    // ⚠ PUBLISHING HERE IS NOT A SECOND WAY TO EARN. Everything it publishes is
    // already in this character's unlock set, which the co-save is the source
    // of, so the save is still the commitment. What it removes is the wait: a
    // character whose colours were committed two saves ago no longer has to
    // save AGAIN before another character can see them. Charge and deeds are
    // not in the file and cannot be, which SharedDyeUnlocks.h argues at length.
    //
    // ⚠⚠ a_publish FALSE MEANS TAKE ONLY, and the asymmetry is the anti-exploit
    // rule rather than an optimisation. TAKING is always free: the file holds
    // ids other characters already banked, and reading them costs this
    // character nothing. PUBLISHING is not, because a colour promotion granted
    // THIS session is not banked until a save, and a deed-gated one was bought
    // with Seamstone charge. Publish it from a live pass and the player can
    // pay, publish, reload to get the charge back, and keep the colour account
    // wide, which is OS-172 rebuilt.
    //
    // So: publish at the save and at the deliberate act of switching the
    // setting on. Take anywhere, as often as is useful.
    //
    // MAIN THREAD ONLY, for SharedDyeUnlocks' unreadable latch.
    void SyncSharedDyeUnlocks(const char* a_reason, bool a_publish = true);

    // The same reconciliation for the appearance collection: take the looks
    // other characters found, then publish the ones this character knows.
    // Everything SyncSharedDyeUnlocks' note says applies unchanged.
    //
    // ⚠ IT PUBLISHES LOOKS AND TAKES LOOKS, AND NEVER TOUCHES THE SEEN-MARKS.
    // An inherited look arrives unacknowledged so the player has something to
    // find; SharedCollection.h carries the argument.
    //
    // a_publish carries SyncSharedDyeUnlocks' meaning. A look is earned by
    // owning the item and a reload takes the item back with it, so the exploit
    // shape is weaker here than for colours; the flag is kept anyway so the two
    // halves of one feature cannot answer the same question differently.
    //
    // MAIN THREAD ONLY.
    void SyncSharedCollection(const char* a_reason, bool a_publish = true);
}
