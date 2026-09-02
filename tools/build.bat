@echo off
call :main > "%~dp0build.log" 2>&1
exit /b %errorlevel%

:main
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if %errorlevel% neq 0 goto :fail
set "VCPKG_ROOT=C:\Users\Maarten\vcpkg"
set "PATH=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
set "CM=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
cd /d "%~dp0.."
echo === CONFIGURE START ===
rem ⚠⚠ THIS BUILD NEVER REACHES THE GAME, AND THAT IS UNCONDITIONAL NOW. The
rem deploy always lands in build\deploy, and tools\redeploy.ps1 is the only thing
rem that puts a DLL anywhere the game can load it. Nothing about where this tree
rem sits on disk changes that.
rem
rem It used to be decided by where the file was run from: a tree under
rem .claude\worktrees\ quarantined, and anything else took the CMakeLists
rem default, which pointed straight at the live MO2 folder. That rule made sense
rem while the worktrees existed and a normal checkout meant the release line. The
rem worktrees were retired on 2026-08-13 and the release line now IS a normal
rem checkout, so the same rule would have deployed the dev DLL and a whole
rem copy_directory of dist over the shared base "Fitting Room" mod. That mod
rem carries the esp, the dye packs and the unlock rules, and it is deliberately
rem DLL free so every slot above it wins the plugin.
rem
rem ⚠ THE OLD RULE FAILED OPEN ONCE ALREADY, which is the argument for having no
rem rule rather than a better one. The first cut tested the path with
rem     findstr /I /L /C:".claude\worktrees\"
rem which never matches: the needle ends in \" and the argument parser reads that
rem as an escaped quote, so errorlevel was always 1. It took the normal-checkout
rem branch inside a worktree and deployed straight into the live Fitting Room
rem mod. A quarantine that depends on a string test is a quarantine that is one
rem parsing quirk away from being off.
rem
rem The release still ships out of dist. tools/make_fomod.sh stages it with one
rem recursive copy of dist and has never read build\deploy, so nothing about
rem shipping needed the live-install branch that used to be here.
rem
rem ⚠ FR_BODY_STUDIO IS PASSED EXPLICITLY, never omitted. It is a cached option,
rem so a tree that was once configured with it OFF keeps OFF through every later
rem configure that stays silent, and one configured with it ON keeps ON. Saying
rem it out loud on every configure is what stops a stale cache deciding whether
rem the feature is in the binary. It is ON for both the dev line and shipping:
rem the 3BA/UBE/HIMBO ownership handoff was field run and the user approved
rem shipping it on 2026-08-08.
rem
rem ⚠ THIS IS NOT THE ISOLATED CHANNEL. FR_BODY_STUDIO_DEV is left alone here,
rem so this build keeps Data/SKSE/Plugins/FittingRoom, the live FittingRoom.ini
rem and 'OSLT' as its co-save owner, and goes on reading outfits it already
rem wrote. tools/build_body_studio_dev.bat is the one that moves all of that.
echo   deploy quarantined to build\deploy, Body Studio ON
"%CM%" --preset release -DOUTPUT_FOLDER="%~dp0..\build\deploy" -DFR_BODY_STUDIO=ON
if %errorlevel% neq 0 goto :fail
echo === BUILD START ===
"%CM%" --build build/release
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
build\release\BodyFitTests.exe
if %errorlevel% neq 0 goto :fail
build\release\BodyMeshPathTests.exe
if %errorlevel% neq 0 goto :fail
build\release\BodySlideCatalogTests.exe
if %errorlevel% neq 0 goto :fail
build\release\BodyPresetStoreTests.exe
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
build\release\HostGuardTests.exe
if %errorlevel% neq 0 goto :fail
rem ⚠ FOUND MISSING 2026-08-15, NOT NEW. FramePolicyTests has been built by
rem every configure since it was added and run by NOBODY: it was the one target
rem on disk that this list did not name, so "carved or plain" was covered by a
rem suite that only ever passed when somebody remembered to type it. A test
rem that never runs is worse than no test, because it reads as coverage.
build\release\FramePolicyTests.exe
if %errorlevel% neq 0 goto :fail
build\release\TextEntryTests.exe
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
build\release\HeadPartLadderTests.exe
if %errorlevel% neq 0 goto :fail
build\release\MeasuredGhostsTests.exe
if %errorlevel% neq 0 goto :fail
build\release\HeadPartSlotPlanTests.exe
if %errorlevel% neq 0 goto :fail
build\release\HeadShotPlanTests.exe
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
build\release\OutfitTabStripTests.exe
if %errorlevel% neq 0 goto :fail
build\release\UiScaleMigrationTests.exe
if %errorlevel% neq 0 goto :fail
build\release\PresetTooltipTests.exe
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
build\release\PresetSceneTests.exe
if %errorlevel% neq 0 goto :fail
build\release\PreviewScopesTests.exe
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
build\release\SharedDyeUnlocksTests.exe
if %errorlevel% neq 0 goto :fail
build\release\DyeHistoryTests.exe
if %errorlevel% neq 0 goto :fail
build\release\DyeRulesTests.exe
if %errorlevel% neq 0 goto :fail
build\release\DyeRequirementsTests.exe
if %errorlevel% neq 0 goto :fail
build\release\DyePromotionTests.exe
if %errorlevel% neq 0 goto :fail
build\release\DyeCardQueueTests.exe
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
rem The FramePolicy lesson, paid again 2026-08-22: these were built by every
rem configure and run by NOBODY until the list was diffed against the folder. A
rem test that never runs reads as coverage.
rem
rem ⚠⚠ AND THE OPPOSITE FAILURE HAPPENED HERE ON 2026-08-25. This list
rem still named OverlayProbePlanTests after 47f7f42 deleted the suite, and
rem build\release\OverlayProbePlanTests.exe was still sitting in the folder from
rem the configure before that commit. So the line did not fail the build: it ran
rem a dead binary against deleted source and printed a pass. build_tests.bat had
rem already been corrected to 83 and this file had not, which is the seventh time
rem these lists have drifted apart. Diff all three, CMakeLists included, and
rem delete the orphaned exe when a suite goes, or the next one passes green too.
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
echo === ALL_DONE ===
exit /b 0

:fail
echo ***BUILD_FAILED*** errorlevel %errorlevel%
exit /b 1
