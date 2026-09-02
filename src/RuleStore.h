#pragma once

// Rules live per SAVE (the 'RULE' co-save record); rules.json is the seed a
// new game starts from and a mirror of the current save. This mirrors the
// outfit library exactly - see Persistence.h - and for the same reason: rules
// name outfits, outfits are per-save, so a global rules file would orphan the
// moment a second character existed.

#include "RuleModel.h"

#include <functional>
#include <string>
#include <vector>

namespace OS::RuleStore {

    // Per-save rule-engine state that rides alongside the rule set in the
    // 'RULE' co-save record (Task 10): whether auto-switching is on,
    // whether the user pinned a look (and to which outfit), and the id of
    // the rule that was applied when this save was last written (the
    // overlay reconstruction anchor - see the design doc's Application
    // path). No RuleEngine instance owns this yet - Task 11 (WorldWatch) is
    // what creates one - so it lives here as the record's staging area:
    // Task 10 fills it from the decoded 'RULE' record (or leaves it at
    // these defaults when the record is absent or rejected) and reads it
    // back on save; Task 11 will read it once at OnSaveLoaded to seed its
    // RuleEngine's pin and toggle, and write updates back here whenever
    // either changes, so the NEXT save's record reflects them.
    //
    // ⚠ TWO-OWNER FIELD, NOT A CACHE WITH ENFORCEMENT: `pinned`/`pinnedName`
    // here and `RuleEngine::pinned_`/`pinnedName_` (RuleEngine.h) are two
    // separate variables with identical meaning and no compiler-checked
    // link between them once Task 11 exists. RuleEngine is the LIVE
    // authority while a session runs; this struct is the SERIALIZATION
    // mirror, read at load and written back on every pin/toggle change.
    // The sync must happen in exactly ONE place (Task 11's read-and-
    // write-back contract) - a second writer, or a SetPinned call that
    // forgets to write back here, leaves this struct invisibly stale, and
    // the next save would then persist a pin nobody currently has (or miss
    // one the player just set). A save-time equality check against the
    // live RuleEngine would be the belt-and-braces addition once Task 11
    // gives this file something to compare against.
    struct EngineState {
        bool        engineEnabled{ true };
        bool        pinned{ false };
        std::string pinnedName;
        std::string lastAppliedRuleId;

        friend bool operator==(const EngineState&, const EngineState&) = default;
    };

    // A consistent copy of the save's engine state. Safe from any thread.
    [[nodiscard]] EngineState GetEngineState();

    // Overwrite the save's engine state (Task 10's load path, once a
    // 'RULE' record decodes cleanly). Independent of WithRules/g_current -
    // none of these fields are written to rules.json, only the 'RULE'
    // co-save record carries them, so this does not queue a mirror save.
    // Safe from any thread.
    void SetEngineState(EngineState a_state);

    // Ids of every currently loaded pack rule the user has switched off
    // (Task 10's save path - the counterpart to SetPackRuleEnabled below).
    // Safe from any thread.
    [[nodiscard]] std::vector<std::string> DisabledPackRuleIds();

    // Renaming an outfit (Task 10; the spec's Persistence section) rewrites
    // every save-owned rule whose base names a_from, and this struct's OWN
    // pinned-outfit mirror too if it names a_from - rules and the pin both
    // name outfits by STRING, so a rename elsewhere would otherwise silently
    // detach them (a base picker pointing at an outfit that no longer
    // exists under that name, or a pin banner naming a look nobody can
    // find). Never touches author packs - they are read-only and cannot
    // name a per-save outfit by definition. A no-op (nothing renamed,
    // nothing pinned under that name) does not queue a write. Safe from any
    // thread.
    //
    // Does NOT touch a live RuleEngine's pin - only this serialization
    // mirror (Task 11/WorldWatch may not have a session running yet, e.g.
    // at co-save decode). A caller inside a live session (the editor) must
    // go through WorldWatch::NotifyOutfitRenamed instead of this function
    // directly, or the LIVE pin - the paused banner's actual source of
    // truth while a session runs - silently keeps the pre-rename name.
    void RenameOutfitEverywhere(const std::string& a_from, const std::string& a_to);

    // Locked access to the save's own rules, for the editor's mutations.
    // EditorUI::Draw runs on ImGuiOverlay::PresentThunk (the RENDER thread,
    // not the main thread), so this cannot be a raw reference - the callback
    // runs while the internal lock is held, mirroring
    // OutfitSession::WithLibrary. Do NOT call another RuleStore function that
    // also takes the lock from inside a_fn (self-deadlock). Every call queues
    // a rules.json mirror save afterward, same convention as WithLibrary
    // queuing a library save; readers use Snapshot() instead. Safe to call
    // from any thread.
    void WithRules(const std::function<void(Rules::RuleSet&)>& a_fn);

    // A consistent copy of the save's own rules for a reader that needs to
    // hold rule state across a frame or across threads without holding the
    // lock (mirrors OutfitSession::SnapshotLibrary). Safe to call from any
    // thread.
    [[nodiscard]] Rules::RuleSet Snapshot();

    // The evaluation list: the save's rules, then author packs in sorted
    // filename order. Array position here IS the priority tie-break, so the
    // order is load-bearing, not cosmetic. Safe to call from any thread (it
    // takes the internal locks itself); called every WorldWatch heartbeat, so
    // it must stay cheap and must not log per call (see the .cpp).
    [[nodiscard]] Rules::RuleSet Merged();

    // Read-only packs from Data/SKSE/Plugins/FittingRoom/Rules/*.json.
    // Call once at data load. Safe to call again later (a future rescan
    // button), but doing so resets every SetPackRuleEnabled override back to
    // each pack's authored default - if a save is loaded when that happens,
    // its disabled-pack-id list must be reapplied immediately afterward or
    // the toggle is silently lost.
    void ScanPacks();

    // Pack rules the user switched off, persisted in the 'RULE' record
    // because the pack files themselves are read-only. Safe to call from any
    // thread; a no-op (logged) if no loaded pack rule has this id.
    void SetPackRuleEnabled(const std::string& a_id, bool a_enabled);

    // Debounced mirror of the current save's rules to rules.json. ALWAYS
    // paired with the library mirror so the two seed files describe the same
    // character (see the spec's Persistence section). Safe to call from any
    // thread. A no-op (logged at debug) until a save is actually loaded -
    // see OnSaveLoaded/OnNewGame/OnRevert below - so the startup and revert
    // windows, where the save's rules are legitimately empty, can never
    // truncate a good rules.json to "no rules".
    //
    // a_pairRules is an internal knob, not for external callers: it is how
    // Persistence::QueueLibrarySave's own paired call INTO this function
    // passes false, making the pairing STRUCTURALLY one hop deep rather
    // than merely safe by debounce-flag ordering - see that function's own
    // doc comment in Persistence.h for the full reasoning.
    void QueueMirrorSave(bool a_pairRules = true);

    // The rules.json seed, for a new game and for the "Load shared rules"
    // button. Returns false and logs when the file is absent, unreadable, or
    // rejected (the file is then renamed aside to rules.json.bad so it is
    // not silently overwritten by the next save). Safe to call from any
    // thread; touches no shared state.
    bool LoadSeed(Rules::RuleSet& a_out);

    // Save lifecycle. Called from the co-save load/revert callbacks (Task
    // 10); safe to call from any thread since each takes the internal locks,
    // but in practice always invoked from the main thread alongside the rest
    // of the save pipeline. Each one also arms or disarms the QueueMirrorSave
    // latch above.
    void OnSaveLoaded(Rules::RuleSet a_fromCoSave);
    void OnNewGame();
    void OnRevert();

}  // namespace OS::RuleStore
