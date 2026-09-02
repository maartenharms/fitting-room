@echo off
REM Builds and runs the pure-logic test executables ONLY.
REM Deliberately never builds the FittingRoom target, whose POST_BUILD step
REM copies the DLL into the live game folder.
REM
REM The target list must stay the SAME eighty-four suites tools/build.bat runs. It
REM
REM ⚠ THE COUNT ABOVE WAS ITSELF STALE ON 2026-08-25, reading eighty-four while
REM the list held eighty-three. The tripwire is only a tripwire if it is
REM corrected in the same edit that adds a suite, and that one was not. It is
REM accurate again because PagePolicyTests landed; check it, do not trust it.
REM held two of them until 2026-07-31, so eight suites reported green here without
REM being built or run at all: a dye case added to test_refreshgate.cpp passed
REM this script while the exe on disk was still the previous build's. Add a
REM suite to CMakeLists and it goes in both scripts or neither.
REM
REM ⚠ IT DRIFTED AGAIN AND THE COUNT ABOVE IS WHY IT WENT UNNOTICED. Found at the
REM 2026-08-03 npc-weapon-transmog merge, seven behind: DyeHistoryTests, four from
REM feat/auto-rules (RuleModel, RuleEngine, RuleCodec, AdvancedConditionParse) and
REM two from this merge (NpcHairPlan, FsmpBridge). Every one arrived on a branch
REM whose author edited build.bat and never opened this file. A merge is exactly
REM when this list goes stale, so check it at every merge, not at every commit.
REM
REM ⚠ AND AGAIN, TEN BEHIND, found 2026-08-06 at OS-161. Missing were the three
REM BuildChannel suites, the four Body ones, ApparelPreviewWire, DyeFlash and
REM SeamstoneCharge - so a quarter of the suite count printed ALL_DONE here
REM without being built. Note what the previous two notes have in common with
REM this one: the drift is always found by someone ADDING a suite, never by the
REM script, because a missing target cannot fail a run it was never in. The
REM count in the line above is the only tripwire, so correct it when you add
REM one. Verify with: grep -c "\.exe$" build.bat against this file.
REM
REM ⚠ AND A THIRD TIME, FIVE BEHIND, found 2026-08-09 while adding
REM DyePreviewTests: BodyFitTests, BodyMeshPathTests, TutorialPlanTests,
REM DyeRampTests and DyeKeyTests were in CMake and build.bat and not here.
REM Synced in the same edit, same lesson as the two notes above.
REM
REM ⚠ AND A FOURTH TIME, FIVE BEHIND AGAIN, found 2026-08-12 while adding
REM DwellAckTests: TextEntry, HeadPartLadder, PresetScene, PreviewScopes and
REM SharedDyeUnlocks were in CMake and build.bat and not here. Four notes now
REM say the same thing, so read the count above as a claim to CHECK rather than
REM as a fact: it was "forty-nine" while build.bat ran fifty-four.
REM
REM ⚠ AND A FIFTH TIME, SEVEN BEHIND, found 2026-08-25 while adding
REM MeasuredGhostsTests: OverlayProbePlan, PortraitPlan, PresetBrowse,
REM ProfileCodec, ProfilePlan, ProfileStore and SkinBindPose were in the RUN
REM list here and in build.bat, and in neither --target line, so all seven
REM printed a verdict off whatever exe the last full build.bat left on disk.
REM Five notes, five times found by someone adding a suite, never by the
REM script. The check is one line and it is written out at the end of the note
REM above: compare the two lists in THIS file, not just this file against
REM build.bat.
REM
REM The failure checks are "if %errorlevel% neq 0" and NOT "if errorlevel 1".
REM That is the second way green has meant nothing here, found 2026-08-01.
REM "if errorlevel N" is a SIGNED >= N comparison, and every Windows crash code
REM is an NTSTATUS of the form 0xC00000xx, which is negative as a signed 32-bit
REM int. So a suite that CRASHED rather than returning 1 did not trip the check
REM and the script walked on and printed ALL_DONE. Proven with a real one:
REM DyePaletteTests died on an uncaught Json::LogicError with 0xC0000409, this
REM script reported exit 0 and ALL_DONE, and the only trace was the absence of
REM that suite's own "all passed" line. An access violation, a stack overflow,
REM heap corruption, an assert and an abort all present the same way. Do not
REM revert these to "if errorlevel 1".
call :main > "%~dp0build_tests.log" 2>&1
exit /b %errorlevel%

:main
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if %errorlevel% neq 0 goto :fail
set "VCPKG_ROOT=C:\Users\Maarten\vcpkg"
set "PATH=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
set "CM=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
cd /d "%~dp0.."
echo === CONFIGURE START ===
"%CM%" --preset release
if %errorlevel% neq 0 goto :fail
echo === BUILD START ===
"%CM%" --build build/release --target BuildChannelReleaseTests BuildChannelDevTests BuildChannelFeatureTests BodyStudioProofDataTests BodyMorphPlanTests BodySlideCatalogTests BodyPresetStoreTests BodyFitTests BodyMeshPathTests OutfitTests PersistenceTests JsonCodecTests SetDetectorTests ShapeOverlayTests OverlayPlanTests SkinPlanTests OverlayTransformTests OverlayLocationsTests MakeupPlanTests BodyMorphCatalogTests BodyMorphTriTests DyeCardQueueTests FramePolicyTests HeadPartSlotPlanTests HeadShotPlanTests HostGuardTests OutfitTabStripTests UiScaleMigrationTests PresetTooltipTests PagePolicyTests EditorGateTests TutorialPlanTests KeyboardArbiterTests WeaponSlotsTests NpcSessionTests NpcHairPlanTests HeadPartPlanTests FsmpBridgeTests RefreshGateTests DyePreviewTests DwellAckTests FaceWaitTests ChamferPolicyTests UndoRedoPlanTests ApparelPreviewWireTests DyeFlashTests RequipFlourishTests RequipDiffTests DyeGateTests DyeQualityTests DyeRampTests DyeBlendTests DyeKeyTests DyeStrengthTests PbrPearlTests SeamstoneChargeTests DyeSchemesTests DyePaletteTests MyDyesTests PreviewGridTests DyeGridTests DyeConditionsTests DyeStatDeedTests DyeUnlocksTests DyeHistoryTests DyeRulesTests DyeRequirementsTests DyePromotionTests ShowcaseTabsTests RuleModelTests RuleEngineTests RuleCodecTests AdvancedConditionParseTests TextEntryTests HeadPartLadderTests PresetSceneTests PreviewScopesTests SharedDyeUnlocksTests DirectEntryTests MeasuredGhostsTests PortraitPlanTests PresetBrowseTests ProfileCodecTests ProfilePlanTests ProfileStoreTests SkinBindPoseTests
if %errorlevel% neq 0 goto :fail
echo === TESTS START ===
build\release\BuildChannelReleaseTests.exe
if %errorlevel% neq 0 goto :fail
build\release\BuildChannelDevTests.exe
if %errorlevel% neq 0 goto :fail
build\release\BuildChannelFeatureTests.exe
if %errorlevel% neq 0 goto :fail
build\release\BodyStudioProofDataTests.exe
if %errorlevel% neq 0 goto :fail
build\release\BodyMorphPlanTests.exe
if %errorlevel% neq 0 goto :fail
build\release\BodySlideCatalogTests.exe
if %errorlevel% neq 0 goto :fail
build\release\BodyPresetStoreTests.exe
if %errorlevel% neq 0 goto :fail
build\release\BodyFitTests.exe
if %errorlevel% neq 0 goto :fail
build\release\BodyMeshPathTests.exe
if %errorlevel% neq 0 goto :fail
build\release\OutfitTests.exe
if %errorlevel% neq 0 goto :fail
build\release\PersistenceTests.exe
if %errorlevel% neq 0 goto :fail
build\release\JsonCodecTests.exe
if %errorlevel% neq 0 goto :fail
build\release\SetDetectorTests.exe
if %errorlevel% neq 0 goto :fail
build\release\ShapeOverlayTests.exe
if %errorlevel% neq 0 goto :fail
build\release\OverlayPlanTests.exe
if %errorlevel% neq 0 goto :fail
build\release\SkinPlanTests.exe
if %errorlevel% neq 0 goto :fail
build\release\OverlayTransformTests.exe
if %errorlevel% neq 0 goto :fail
build\release\OverlayLocationsTests.exe
if %errorlevel% neq 0 goto :fail
build\release\MakeupPlanTests.exe
if %errorlevel% neq 0 goto :fail
build\release\BodyMorphCatalogTests.exe
if %errorlevel% neq 0 goto :fail
build\release\BodyMorphTriTests.exe
if %errorlevel% neq 0 goto :fail
build\release\PagePolicyTests.exe
if %errorlevel% neq 0 goto :fail
build\release\EditorGateTests.exe
if %errorlevel% neq 0 goto :fail
build\release\TutorialPlanTests.exe
if %errorlevel% neq 0 goto :fail
build\release\KeyboardArbiterTests.exe
if %errorlevel% neq 0 goto :fail
build\release\WeaponSlotsTests.exe
if %errorlevel% neq 0 goto :fail
build\release\NpcSessionTests.exe
if %errorlevel% neq 0 goto :fail
build\release\NpcHairPlanTests.exe
if %errorlevel% neq 0 goto :fail
build\release\HeadPartPlanTests.exe
if %errorlevel% neq 0 goto :fail
build\release\FsmpBridgeTests.exe
if %errorlevel% neq 0 goto :fail
build\release\RefreshGateTests.exe
if %errorlevel% neq 0 goto :fail
build\release\ApparelPreviewWireTests.exe
if %errorlevel% neq 0 goto :fail
build\release\DyeFlashTests.exe
if %errorlevel% neq 0 goto :fail
build\release\RequipFlourishTests.exe
if %errorlevel% neq 0 goto :fail
build\release\RequipDiffTests.exe
if %errorlevel% neq 0 goto :fail
build\release\DyePreviewTests.exe
if %errorlevel% neq 0 goto :fail
build\release\DwellAckTests.exe
if %errorlevel% neq 0 goto :fail
build\release\FaceWaitTests.exe
if %errorlevel% neq 0 goto :fail
build\release\ChamferPolicyTests.exe
if %errorlevel% neq 0 goto :fail
build\release\UndoRedoPlanTests.exe
if %errorlevel% neq 0 goto :fail
build\release\DyeGateTests.exe
if %errorlevel% neq 0 goto :fail
build\release\DyeQualityTests.exe
if %errorlevel% neq 0 goto :fail
build\release\DyeRampTests.exe
if %errorlevel% neq 0 goto :fail
build\release\DyeBlendTests.exe
if %errorlevel% neq 0 goto :fail
build\release\DyeKeyTests.exe
if %errorlevel% neq 0 goto :fail
build\release\DyeStrengthTests.exe
if %errorlevel% neq 0 goto :fail
build\release\PbrPearlTests.exe
if %errorlevel% neq 0 goto :fail
build\release\SeamstoneChargeTests.exe
if %errorlevel% neq 0 goto :fail
build\release\DyeSchemesTests.exe
if %errorlevel% neq 0 goto :fail
build\release\DyePaletteTests.exe
if %errorlevel% neq 0 goto :fail
build\release\MyDyesTests.exe
if %errorlevel% neq 0 goto :fail
build\release\PreviewGridTests.exe
if %errorlevel% neq 0 goto :fail
build\release\DyeGridTests.exe
if %errorlevel% neq 0 goto :fail
build\release\DyeConditionsTests.exe
if %errorlevel% neq 0 goto :fail
build\release\DyeStatDeedTests.exe
if %errorlevel% neq 0 goto :fail
build\release\DyeUnlocksTests.exe
if %errorlevel% neq 0 goto :fail
build\release\DyeHistoryTests.exe
if %errorlevel% neq 0 goto :fail
build\release\DyeRulesTests.exe
if %errorlevel% neq 0 goto :fail
build\release\DyeRequirementsTests.exe
if %errorlevel% neq 0 goto :fail
build\release\DyePromotionTests.exe
if %errorlevel% neq 0 goto :fail
build\release\ShowcaseTabsTests.exe
if %errorlevel% neq 0 goto :fail
build\release\RuleModelTests.exe
if %errorlevel% neq 0 goto :fail
build\release\RuleEngineTests.exe
if %errorlevel% neq 0 goto :fail
build\release\RuleCodecTests.exe
if %errorlevel% neq 0 goto :fail
build\release\AdvancedConditionParseTests.exe
if errorlevel 1 goto :fail
rem The FramePolicy lesson, paid again 2026-08-22: these six were built
rem by every configure and run by NOBODY until the list was diffed against
rem the folder. A test that never runs reads as coverage.
build\release\ProfileCodecTests.exe
if %errorlevel% neq 0 goto :fail
build\release\ProfilePlanTests.exe
if %errorlevel% neq 0 goto :fail
build\release\ProfileStoreTests.exe
if %errorlevel% neq 0 goto :fail
build\release\SkinBindPoseTests.exe
if %errorlevel% neq 0 goto :fail
build\release\PresetBrowseTests.exe
if %errorlevel% neq 0 goto :fail
build\release\PortraitPlanTests.exe
if %errorlevel% neq 0 goto :fail
build\release\DirectEntryTests.exe
if %errorlevel% neq 0 goto :fail
build\release\TextEntryTests.exe
if %errorlevel% neq 0 goto :fail
build\release\HeadPartLadderTests.exe
if %errorlevel% neq 0 goto :fail
build\release\MeasuredGhostsTests.exe
if %errorlevel% neq 0 goto :fail
build\release\PresetSceneTests.exe
if %errorlevel% neq 0 goto :fail
build\release\PreviewScopesTests.exe
if %errorlevel% neq 0 goto :fail
build\release\SharedDyeUnlocksTests.exe
if %errorlevel% neq 0 goto :fail
REM ⚠ AND A FIFTH TIME, SIX BEHIND, found 2026-08-16 while adding
REM BodyMorphTriTests. Missing were DyeCardQueue, FramePolicy, HeadPartSlotPlan,
REM HostGuard, OutfitTabStrip and PresetTooltip. Found the way all four previous
REM ones were: by someone ADDING a suite and counting, never by the script.
REM build.bat cannot drift this way because it has no target list at all - it
REM runs a bare "--build build/release" and every add_executable comes with it.
REM This file names its targets, so this file is the only one that can go stale,
REM and the count in the header is the only tripwire.
REM Check with: grep -o '[A-Za-z0-9_]*\.exe$' on both files and diff them.
build\release\DyeCardQueueTests.exe
if %errorlevel% neq 0 goto :fail
build\release\FramePolicyTests.exe
if %errorlevel% neq 0 goto :fail
build\release\HeadPartSlotPlanTests.exe
if %errorlevel% neq 0 goto :fail
build\release\HeadShotPlanTests.exe
if %errorlevel% neq 0 goto :fail
build\release\HostGuardTests.exe
if %errorlevel% neq 0 goto :fail
build\release\OutfitTabStripTests.exe
if %errorlevel% neq 0 goto :fail
build\release\UiScaleMigrationTests.exe
if %errorlevel% neq 0 goto :fail
build\release\PresetTooltipTests.exe
if %errorlevel% neq 0 goto :fail
echo === ALL_DONE ===
exit /b 0

:fail
echo ***BUILD_FAILED*** errorlevel %errorlevel%
exit /b 1
