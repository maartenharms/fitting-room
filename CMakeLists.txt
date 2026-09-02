cmake_minimum_required(VERSION 3.21)

# "Fitting Room" (formerly Outfit Slots) - ESO-style transmog with saved outfit slots.
# One DLL supports Skyrim SE 1.5.97 and next-gen AE 1.6.1130+.
project(FittingRoom VERSION 1.1.7 LANGUAGES CXX)

# ⚠ TWO OPTIONS, NOT ONE. FR_BODY_STUDIO compiles the authoring workbench in.
# FR_BODY_STUDIO_DEV additionally moves the build onto an isolated data channel
# (its own data root, INI, log and export names, and 'FBSD' as the co-save
# owner instead of 'OSLT'). The channel implies the feature; the feature does
# not imply the channel, which is what lets the development line ship one build
# holding the dye work AND Body Studio while still reading the live INI and the
# outfits already written under 'OSLT'. See src/BuildChannel.h.
#
# FR_BODY_STUDIO DEFAULTS ON AS OF 2026-08-08, and it used to default OFF. It
# was held back until the 3BA/UBE/HIMBO ownership handoff had been exercised,
# because BodyStudioProof unassigns OBody's preset and takes the RaceMenu morph
# key itself, and a body family that had never been through that path was a
# body family nobody knew it worked on. All three have now been run in the
# field and the user approved shipping it (2026-08-08).
#
# ⚠ FR_BODY_STUDIO_DEV STILL DEFAULTS OFF AND IS THE ONE THAT MATTERS FOR DATA.
# It is the isolated channel: its own data root, INI, log, export names, and
# 'FBSD' as the co-save owner. The feature shipping publicly says nothing about
# the channel, and turning the channel on for a release would strand every
# outfit ever written under 'OSLT'.
option(FR_BODY_STUDIO "Compile the Body Studio authoring workbench into the plugin" ON)
option(FR_BODY_STUDIO_DEV "Build the isolated Body Studio development channel" OFF)
set(FR_BUILD_ID "release" CACHE STRING "Build/channel identifier embedded in development artifacts")
if(FR_BODY_STUDIO_DEV)
    if(FR_BUILD_ID STREQUAL "" OR FR_BUILD_ID STREQUAL "release")
        message(FATAL_ERROR "FR_BODY_STUDIO_DEV requires a non-release FR_BUILD_ID")
    endif()
    # FORCE, because the cache entry from an earlier configure of this same
    # tree may hold OFF and would otherwise win over a plain set().
    set(FR_BODY_STUDIO ON CACHE BOOL "Compile the Body Studio authoring workbench into the plugin" FORCE)
    set(FR_DATA_SUBDIR "FittingRoom.BodyStudioDev/${FR_BUILD_ID}")
else()
    set(FR_DATA_SUBDIR "FittingRoom")
endif()

find_package(CommonLibSSE CONFIG REQUIRED)
# jsoncpp's exported config predates CMake 3.5's policy floor.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
find_package(jsoncpp CONFIG REQUIRED)
unset(CMAKE_POLICY_VERSION_MINIMUM)
find_package(imgui CONFIG REQUIRED)
find_package(tinyxml2 CONFIG REQUIRED)
# ⚠ NAMED HERE SO A TEST TARGET CAN LINK IT. The plugin picks spdlog up through
# CommonLibSSE and its precompiled header, but a test executable carries neither,
# so a source shared between the two cannot log without this. BodySlideCatalog
# went 663 lines with no logging at all partly because adding one line did not
# build.
find_package(spdlog CONFIG REQUIRED)
# The overlay picker's thumbnails. WIC, which PreviewPng already encodes
# through, decodes DXT1 and DXT5 and REFUSES BC7; measured 2026-08-16, BC7 is
# 595 of the 1851 overlay textures on the reference load order. This decodes
# every block format on the CPU, so the work stays off the render thread and
# away from the game's immediate context.
find_package(directxtex CONFIG REQUIRED)
# OS-154: the ONLY dependency here that exists to hook a function ENTRY. Every
# other Fitting Room hook patches a call SITE, which CommonLib's trampoline does
# natively. The item-preview appender cannot be reached that way: it is
# non-virtual, and callers differ per runtime, so a call-site hook would be a
# handful of patches that silently under-cover when one changes shape.
# SafetyHook relocates the stolen prologue, which CommonLib's write_branch does
# NOT do, and that is what makes one entry detour possible.
# ⚠ zydis is pinned to 4.1.1 on purpose: safetyhook 0.7.0 needs the 4.x decoder
# API and the vcpkg baseline resolves 3.2.1, which fails to compile with
# "ZYDIS_STACK_WIDTH_64: undeclared identifier". Both need overrides entries
# because the baseline predates the safetyhook port.
find_package(safetyhook CONFIG REQUIRED)

# ⚠ A CRASH LOG THAT CANNOT NAME OUR BUILD COSTS A DIAGNOSIS. This puts the
# version into the DLL itself, so Crash Logger and Trainwreck print it beside
# our name like every other plugin. Generated from project() so it cannot
# drift from the other version files.
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.rc.in"
               "${CMAKE_CURRENT_BINARY_DIR}/version.rc" @ONLY)

add_commonlibsse_plugin(${PROJECT_NAME}
    SOURCES
        "${CMAKE_CURRENT_BINARY_DIR}/version.rc"
        src/PCH.h
        src/BuildChannel.h
        src/plugin.cpp
        src/VersionCheck.h
        src/VersionCheck.cpp
        src/VmCall.h
        src/VmCall.cpp
        src/REAugments.h
        src/REAugments.cpp
        src/StyleRef.h
        src/StyleRef.cpp
        src/StyleCatalog.h
        src/StyleCatalog.cpp
        src/SetDetector.h
        src/SetDetector.cpp
        src/AutoPresets.h
        src/AutoPresets.cpp
        src/EditorGate.h
        src/FooterNotice.h
        src/OutfitTabs.h
        src/OutfitTabStrip.h
        src/ShowcaseTabs.h
        src/ApparelPreviewSignal.h
        src/SamCompat.h
        src/SamCompat.cpp
        src/MenuStudioCompat.h
        src/MenuStudioCompat.cpp
        src/CrashGuard.h
        src/CrashGuard.cpp
        src/Favorites.h
        src/Favorites.cpp
        src/RecentMods.h
        src/RecentMods.cpp
        src/Icons.h
        src/IconImages.h
        src/IconImages.cpp
        src/Diagnostics.h
        src/Diagnostics.cpp
        src/CameraProbe.h
        src/CameraProbe.cpp
        src/CbpcArmorClass.h
        src/CbpcArmorClass.cpp
        src/CbpcRefresh.h
        src/CbpcRefresh.cpp
        src/FsmpXmlOverrides.h
        src/FsmpXmlOverrides.cpp
        src/PreviewFrame.h
        src/PreviewGrid.h
        src/PresetScene.h
        src/PreviewScopes.h
        src/PreviewScopes.cpp
        src/PreviewManifest.h
        src/NifModelLoader.h
        src/NifModelLoader.cpp
        src/MeshExtractor.h
        src/MeshExtractor.cpp
        src/PreviewRenderer.h
        src/PreviewRenderer.cpp
        src/PreviewPng.h
        src/PreviewPng.cpp
        src/PreviewDiskCache.h
        src/PreviewDiskCache.cpp
        src/PreviewCache.h
        src/PreviewCache.cpp
        src/Mannequin.h
        src/Mannequin.cpp
        src/MannequinPose.h
        src/NpcHair.h
        src/NpcHair.cpp
        # The invisible FUCK ticker that drains NpcHair's settle ladders with
        # the editor shut (cell-return doubled hair, 2026-08-21).
        src/NpcHairSettle.h
        src/NpcHairSettle.cpp
        src/FsmpBridge.h
        src/FsmpBridge.cpp
        # Temporary FSMP-in-FR field probe (2026-08-23); dies with the
        # instrument strip when the hair gates pass.
        src/FsmpProbe.h
        src/FsmpProbe.cpp
        src/MenuStudioApi.h
        src/MenuStudioApi.cpp
        src/CameraFrame.h
        src/CameraFrame.cpp
        src/HostGuard.h
        src/HostGuard.cpp
        src/SceneGuard.h
        src/SceneGuard.cpp
        src/RaceSwitchSink.h
        src/RaceSwitchSink.cpp
        src/NpcLoadSink.h
        src/NpcLoadSink.cpp
        src/HeadEditorSink.h
        src/HeadEditorSink.cpp
        src/Migration.h
        src/OutfitSession.h
        src/OutfitSession.cpp
        src/BipedHooks.h
        src/BipedHooks.cpp
        src/RealWorn.h
        src/BipedPost.h
        src/BipedPost.cpp
        src/WeaponSlots.h
        src/WeaponHooks.h
        src/WeaponHooks.cpp
        src/WeaponPreview.h
        src/WeaponPreview.cpp
        src/InputListener.h
        src/InputListener.cpp
        src/TextEntry.h
        src/TextEntry.cpp
        src/MenuButton.h
        src/MenuButton.cpp
        src/LoreModule.h
        src/LoreModule.cpp
        src/DirectEntry.h
        src/DirectEntry.cpp
        extern/RaceMenu/IPluginInterface.h
        src/RaceMenuMorphApi.h
        src/RaceMenuMorphApi.cpp
        src/ShapeOverlay.h
        src/NodeTransformApi.h
        src/NodeTransformApi.cpp
        src/BodyMorphCatalogParse.h
        src/BodyMorphCatalog.h
        src/BodyMorphCatalog.cpp
        src/BodyMorphPresets.h
        src/BodyMorphPresets.cpp
        src/ShapeUI.h
        src/ShapeUI.cpp
        src/OverlayPlan.h
        src/OverlayApi.h
        src/OverlayApi.cpp
        src/OverlayLocations.h
        src/OverlayLocations.cpp
        src/PaintScrape.h
        src/PaintScrape.cpp
        src/OverlayTextures.h
        src/OverlayTextures.cpp
        src/OverlayThumbs.h
        src/OverlayThumbs.cpp
        src/OverlaysUI.h
        src/OverlaysUI.cpp
        src/OverlayTransform.h
        src/OverlayReconcile.h
        src/CharacterSex.cpp
        src/LookFaceTint.h
        src/LookFaceTint.cpp
        src/MakeupBaseline.h
        src/MakeupBaseline.cpp
        src/OverlayBaseline.cpp
        src/OverlayReconcile.cpp
        src/Overlay1P.h
        src/Overlay1P.cpp
        src/OverlayBake.h
        src/OverlayBake.cpp
        src/TextureLoad.h
        src/TextureLoad.cpp
        src/PresetRequirements.h
        src/SkinImport.h
        src/SkinPlan.h
        src/SkinPacks.h
        src/SkinPacks.cpp
        src/SkinRivals.h
        src/SkinRivals.cpp
        src/SkinApi.h
        src/SkinApi.cpp
        src/MakeupPlan.h
        src/MakeupApi.h
        src/MakeupApi.cpp
        src/RaceTint.h
        src/RaceTint.cpp
        src/ObodyApi.h
        src/ObodyApi.cpp
        extern/OBody_API.h
        src/DefaultLook.h
        src/DefaultLook.cpp
        src/HairColor.h
        src/HairColor.cpp
        src/HeadBuildHook.h
        src/HeadBuildHook.cpp
        src/MaterialReading.h
        src/MaterialReading.cpp
        src/HairStyle.h
        src/HeadPartPlan.h
        src/HeadPart.h
        src/HeadPart.cpp
        src/DyeGate.h
        src/DyeQuality.h
        src/OutfitDye.h
        src/OutfitDye.cpp
        src/Requip.cpp
        src/DyeGpu.h
        src/DyeGpu.cpp
        src/DyeTexture.h
        src/DyeTexture.cpp
        src/KeyboardArbiter.h
        src/ImGuiOverlay.h
        src/ImGuiOverlay.cpp
        src/EditorUI.h
        src/EditorUI.cpp
        src/FuckCompat.h
        src/TutorialPlan.h
        src/Tutorial.h
        src/Tutorial.cpp
        src/EditorWindow.h
        src/EditorWindow.cpp
        src/Inventory3DHooks.h
        src/Inventory3DHooks.cpp
        src/EditorStyle.h
        src/EditorStyle.cpp
        src/JsonCodec.h
        src/JsonCodec.cpp
        src/ProfileCodec.h
        src/ProfileCodec.cpp
        src/ProfilePlan.h
        src/ProfileStore.h
        src/ProfileStore.cpp
        src/ProfileCapture.h
        src/ProfileCapture.cpp
        src/ProfileApply.h
        src/ProfileApply.cpp
        src/ProfilesUI.h
        src/ProfilesUI.cpp
        src/PresetBrowse.h
        src/PresetBrowse.cpp
        src/Portrait.h
        src/Portrait.cpp
        src/PortraitPlan.h
        src/PresetStore.h
        src/PresetStore.cpp
        src/DyeSchemes.h
        src/DyeSchemes.cpp
        src/DyePalette.h
        src/DyePalette.cpp
        src/MyDyes.h
        src/MyDyes.cpp
        src/DyeGrid.h
        src/DyeGrid.cpp
        src/ChamferPolicy.h
        src/ChamferPanel.h
        src/ChamferPanel.cpp
        src/DyeHistory.h
        src/DyeHistory.cpp
        src/DyeConditions.h
        src/DyeConditions.cpp
        src/DyeRequirements.h
        src/DyeRequirements.cpp
        src/DyeRules.h
        src/DyeRules.cpp
        src/DyeUnlocks.h
        src/DyeUnlocks.cpp
        src/SharedCollection.h
        src/SharedCollection.cpp
        src/SharedDyeUnlocks.h
        src/SharedDyeUnlocks.cpp
        src/SeamstoneCharge.h
        src/SoulGems.h
        src/SoulGems.cpp
        src/HookSite.h
        src/SkeeHookCensus.h
        src/AppearanceWatch.h
        src/AppearanceWatch.cpp
        src/ItemCardCharge.h
        src/ItemCardCharge.cpp
        src/SeamstoneRecharge.h
        src/SeamstoneRecharge.cpp
        src/BodyMeshPath.h
        src/BodyMeshPath.cpp
        src/DyeWorld.h
        src/DyeWorld.cpp
        # ⚠ StatProbe.{h,cpp} STOOD HERE AND IS GONE, which is what its own note
        # asked for: it was a spike carrying an instruction to delete itself
        # once the log had answered whether Game.QueryStat is usable from an
        # SKSE plugin. It answered on 2026-08-08 - the read works, the callback
        # lands on the VM's thread ~180 ms later, and a name the engine does not
        # know comes back int 0 exactly like an honest zero. DyeStats below is
        # that answer built out.
        src/DyeStatDeed.h
        src/DyeStats.h
        src/DyeStats.cpp
        # ⚠ A SPIKE, ON THE StatProbe PATTERN: ChargenProbe answered phase 0
        # (all three gates read 2026-08-21) and died with W1, per its own
        # contract. ProfileProbe is its successor on the same key and dies
        # the same way when the Looks page (W2) lands: one press captures
        # the player into "F11 Look", the next applies it back.
        src/ProfileProbe.h
        src/ProfileProbe.cpp
        src/DyePromotion.h
        src/DyePromotion.cpp
        # Telling the player a colour just unlocked (2026-08-14). The queue is
        # pure and headless-tested; the card is a second FUCK IWindow drawn over
        # live gameplay; the tick is what makes there be a moment to announce,
        # because promotion ran only at load and at editor open before this.
        src/DyeCardQueue.h
        src/DyeCardQueue.cpp
        src/DyeUnlockCard.h
        src/DyeUnlockCard.cpp
        src/DyeTick.h
        src/DyeTick.cpp
        src/Persistence.h
        src/Persistence.cpp
        src/Collection.h
        src/Collection.cpp
        src/CostMode.h
        src/Settings.h
        src/Settings.cpp
        src/SettingsUI.h
        src/SettingsUI.cpp
        src/RuleModel.h
        src/RuleEngine.h
        src/RuleEngine.cpp
        src/RuleCodec.h
        src/RuleCodec.cpp
        src/AdvancedConditionParse.h
        src/AdvancedCondition.h
        src/AdvancedCondition.cpp
        src/RuleStore.h
        src/RuleStore.cpp
        src/WorldWatch.h
        src/WorldWatch.cpp
        src/RulesUI.h
        src/RulesUI.cpp
)

if(FR_BODY_STUDIO)
    target_sources(${PROJECT_NAME} PRIVATE
        src/BodyStudioProofData.h
        src/BodyStudioProofData.cpp
        src/BodyPreset.h
        src/BodyMorphPlan.h
        src/BodySlideCatalog.h
        src/BodySlideCatalog.cpp
        src/BodyMorphData.cpp
        src/BodyMorphSource.cpp
        src/BodyCardScene.h
        src/BodyCardScene.cpp
        src/BodyPresetJson.h
        src/BodyPresetJson.cpp
        src/BodyPresetStore.h
        src/BodyPresetStore.cpp
        src/BodyStudioUI.h
        src/BodyStudioUI.cpp
        src/BodyStudioDraft.h
        src/BodyStudioLayout.h
        src/BodyStudioProof.h
        src/BodyStudioProof.cpp
        src/BodyWeight.h
        src/BodyWeight.cpp
        src/DefaultBody.h
        src/DefaultBody.cpp)
endif()

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)
# extern/ holds the vendored FUCK API header (FUCK_API.h, soft dependency,
# resolved at runtime via RequestFUCK / GetProcAddress - no link-time coupling).
target_include_directories(${PROJECT_NAME} PRIVATE extern)
# d3dcompiler serves two runtime-compiled users now: DyeGpu's OS-139 compute
# probe and the preview grid's draw shaders (PreviewRenderer.cpp). The link
# stays until both compile offline.
# windowscodecs is the preview grid's PNG encoder (PreviewPng.cpp), the
# project's first in-process image codec; decode stays FUCK's job.
target_link_libraries(${PROJECT_NAME} PRIVATE JsonCpp::JsonCpp imgui::imgui
                                              safetyhook::safetyhook
                                              Microsoft::DirectXTex
                                              d3d11 dxgi d3dcompiler windowscodecs)
if(FR_BODY_STUDIO)
    target_link_libraries(${PROJECT_NAME} PRIVATE tinyxml2::tinyxml2)
endif()
target_compile_definitions(${PROJECT_NAME} PRIVATE IMGUI_DEFINE_MATH_OPERATORS)
target_compile_definitions(${PROJECT_NAME} PRIVATE
    FR_BUILD_ID="${FR_BUILD_ID}"
    FR_DATA_SUBDIR="${FR_DATA_SUBDIR}"
    $<$<BOOL:${FR_BODY_STUDIO}>:FR_BODY_STUDIO=1>
    $<$<BOOL:${FR_BODY_STUDIO_DEV}>:FR_BODY_STUDIO_DEV=1>)

# Ship debug symbols with the optimised build, so a crash address can be
# turned into a function name.
#
# ⚠ THIS EXISTS BECAUSE ITS ABSENCE COST A DIAGNOSIS. A trainwreck log on
# 2026-08-05 pointed at "FittingRoom.dll+007823C" four frames deep, and with no
# PDB there was no way to say which function that was. Crash loggers read a PDB
# sitting beside the DLL, so shipping it turns every future report from an
# offset into a name and a line.
#
# ⚠ /Zi AND /DEBUG DO NOT CHANGE THE GENERATED CODE, but /DEBUG alone WOULD:
# the linker silently turns /OPT:REF and /OPT:ICF off when /DEBUG is given, so
# they are asked for explicitly. Without them the binary really would be a
# different one from the build that was tested, which is the opposite of what a
# diagnostic aid should do.
if (MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE $<$<NOT:$<CONFIG:Debug>>:/Zi>)
    target_link_options(${PROJECT_NAME} PRIVATE
        $<$<NOT:$<CONFIG:Debug>>:/DEBUG /OPT:REF /OPT:ICF>)
endif ()

# Pure-logic unit tests (no RE/engine deps).
add_executable(BuildChannelReleaseTests tests/test_buildchannel.cpp)
target_compile_features(BuildChannelReleaseTests PRIVATE cxx_std_23)
target_include_directories(BuildChannelReleaseTests PRIVATE src)
target_compile_definitions(BuildChannelReleaseTests PRIVATE FR_TEST_EXPECT_DEV=0)

add_executable(BuildChannelDevTests tests/test_buildchannel.cpp)
target_compile_features(BuildChannelDevTests PRIVATE cxx_std_23)
target_include_directories(BuildChannelDevTests PRIVATE src)
target_compile_definitions(BuildChannelDevTests PRIVATE
    FR_TEST_EXPECT_DEV=1
    FR_BODY_STUDIO_DEV=1
    FR_BUILD_ID="test-build"
    FR_DATA_SUBDIR="FittingRoom.BodyStudioDev/test-build")

# The development line's shape: workbench compiled in, data channel untouched.
# Built unconditionally so the combination is checked even from a release
# configure, since it is the one a build-flag slip would break silently.
add_executable(BuildChannelFeatureTests tests/test_buildchannel.cpp)
target_compile_features(BuildChannelFeatureTests PRIVATE cxx_std_23)
target_include_directories(BuildChannelFeatureTests PRIVATE src)
target_compile_definitions(BuildChannelFeatureTests PRIVATE
    FR_TEST_EXPECT_DEV=0
    FR_BODY_STUDIO=1)

add_executable(BodyStudioProofDataTests
    tests/test_body_studio_proof_data.cpp
    src/BodyStudioProofData.cpp)
target_compile_features(BodyStudioProofDataTests PRIVATE cxx_std_23)
target_include_directories(BodyStudioProofDataTests PRIVATE src)
target_link_libraries(BodyStudioProofDataTests PRIVATE tinyxml2::tinyxml2)

add_executable(BodyMorphPlanTests tests/test_body_morph_plan.cpp)
target_compile_features(BodyMorphPlanTests PRIVATE cxx_std_23)
target_include_directories(BodyMorphPlanTests PRIVATE src)

# Pure-logic body-fit tests. The two pickers that call BodyFitCompatible live
# in EditorUI.cpp and BodyStudioUI.cpp, neither of which any test compiles, so
# the predicate sits in BodyPreset.h and is covered here instead.
add_executable(BodyFitTests tests/test_body_fit.cpp)
target_compile_features(BodyFitTests PRIVATE cxx_std_23)
target_include_directories(BodyFitTests PRIVATE src)

# Which shape our own chrome draws. ⚠ Menu Studio carries the same header and
# the same test: the two plugins draw in one menu, so what a stored iFrameStyle
# resolves to must not drift between them.
add_executable(FramePolicyTests tests/test_frame_policy.cpp)
target_compile_features(FramePolicyTests PRIVATE cxx_std_23)
target_include_directories(FramePolicyTests PRIVATE src)

# Where a skinned mesh stands when its vertex buffer does not say. The engine
# side gathers boneWorld * skinToBone per bone; the decision about whether those
# products agree, and whether there is anything to correct, is pure and lives in
# SkinBindPose.h. Its numbers are measured off two real files rather than chosen.
add_executable(SkinBindPoseTests tests/test_skin_bind_pose.cpp)
target_compile_features(SkinBindPoseTests PRIVATE cxx_std_23)
target_include_directories(SkinBindPoseTests PRIVATE src)

# The body mesh key, which is what lets a race's skin and a BodySlide slider set
# be compared. The engine-reading half of that header lives in the .cpp and is
# not linked here; only the pure path arithmetic is covered, which is the part
# that can be wrong quietly.
add_executable(BodyMeshPathTests tests/test_body_mesh_path.cpp)
target_compile_features(BodyMeshPathTests PRIVATE cxx_std_23)
target_include_directories(BodyMeshPathTests PRIVATE src)

add_executable(BodySlideCatalogTests
    tests/test_body_slide_catalog.cpp
    src/BodySlideCatalog.cpp)
target_compile_features(BodySlideCatalogTests PRIVATE cxx_std_23)
target_include_directories(BodySlideCatalogTests PRIVATE src)
target_link_libraries(BodySlideCatalogTests PRIVATE tinyxml2::tinyxml2 spdlog::spdlog)

add_executable(BodyPresetStoreTests
    tests/test_body_preset_store.cpp
    src/BodyPresetJson.cpp
    src/BodyPresetStore.cpp)
target_compile_features(BodyPresetStoreTests PRIVATE cxx_std_23)
target_include_directories(BodyPresetStoreTests PRIVATE src)
target_compile_definitions(BodyPresetStoreTests PRIVATE
    FR_BODY_STUDIO_DEV=1
    FR_BUILD_ID="test-build"
    FR_DATA_SUBDIR="FittingRoom.BodyStudioDev/test-build")
target_link_libraries(BodyPresetStoreTests PRIVATE JsonCpp::JsonCpp tinyxml2::tinyxml2)

add_executable(OutfitTests tests/test_outfit.cpp)
target_compile_features(OutfitTests PRIVATE cxx_std_23)
target_include_directories(OutfitTests PRIVATE src)

add_executable(PersistenceTests tests/test_persistence.cpp)
target_compile_features(PersistenceTests PRIVATE cxx_std_23)
target_include_directories(PersistenceTests PRIVATE src)

# The JSON codec is pure logic but needs jsoncpp.
add_executable(JsonCodecTests tests/test_jsoncodec.cpp src/JsonCodec.cpp)
target_compile_features(JsonCodecTests PRIVATE cxx_std_23)
target_include_directories(JsonCodecTests PRIVATE src)
target_link_libraries(JsonCodecTests PRIVATE JsonCpp::JsonCpp)

# The profile codec is pure logic too; it embeds the outfit and body preset
# codecs, so their translation units ride along.
add_executable(ProfileCodecTests
    tests/test_profilecodec.cpp
    src/ProfileCodec.cpp
    src/JsonCodec.cpp
    src/BodyPresetJson.cpp)
target_compile_features(ProfileCodecTests PRIVATE cxx_std_23)
target_include_directories(ProfileCodecTests PRIVATE src)
target_link_libraries(ProfileCodecTests PRIVATE JsonCpp::JsonCpp)

# The preset browser's listings and export-name normaliser are pure
# filesystem logic; the fixtures are the rig's own filename zoo, the
# .nif.nif pileup included.
add_executable(PresetBrowseTests
    tests/test_presetbrowse.cpp
    src/PresetBrowse.cpp)
target_compile_features(PresetBrowseTests PRIVATE cxx_std_23)
target_include_directories(PresetBrowseTests PRIVATE src)

# The portrait capture's pure half: crop arithmetic and pixel conversion.
add_executable(PortraitPlanTests tests/test_portraitplan.cpp)
target_compile_features(PortraitPlanTests PRIVATE cxx_std_23)
target_include_directories(PortraitPlanTests PRIVATE src)

# The apply order is the two-painter resolution made executable; the plan is
# header-only pure logic.
add_executable(ProfilePlanTests
    tests/test_profileplan.cpp
    src/ProfileCodec.cpp
    src/JsonCodec.cpp
    src/BodyPresetJson.cpp)
target_compile_features(ProfilePlanTests PRIVATE cxx_std_23)
target_include_directories(ProfilePlanTests PRIVATE src)
target_link_libraries(ProfilePlanTests PRIVATE JsonCpp::JsonCpp)

# The profile store is filesystem CRUD over the profile codec, engine-free
# like the body preset store it mirrors.
add_executable(ProfileStoreTests
    tests/test_profile_store.cpp
    src/ProfileStore.cpp
    src/ProfileCodec.cpp
    src/JsonCodec.cpp
    src/BodyPresetJson.cpp)
target_compile_features(ProfileStoreTests PRIVATE cxx_std_23)
target_include_directories(ProfileStoreTests PRIVATE src)
target_compile_definitions(ProfileStoreTests PRIVATE
    FR_BODY_STUDIO_DEV=1
    FR_BUILD_ID="test-build"
    FR_DATA_SUBDIR="FittingRoom.BodyStudioDev/test-build")
target_link_libraries(ProfileStoreTests PRIVATE JsonCpp::JsonCpp)

# Pure-logic dye scheme tests (the store is JSON on disk, no engine).
add_executable(DyeSchemesTests tests/test_dyeschemes.cpp src/DyeSchemes.cpp src/JsonCodec.cpp)
target_compile_features(DyeSchemesTests PRIVATE cxx_std_23)
target_include_directories(DyeSchemesTests PRIVATE src)
target_link_libraries(DyeSchemesTests PRIVATE JsonCpp::JsonCpp)

# Pure-logic dye palette tests (definitions only, no engine).
add_executable(DyePaletteTests tests/test_dyepalette.cpp src/DyePalette.cpp src/JsonCodec.cpp)
target_compile_features(DyePaletteTests PRIVATE cxx_std_23)
target_include_directories(DyePaletteTests PRIVATE src)
target_link_libraries(DyePaletteTests PRIVATE JsonCpp::JsonCpp)

# Pure-logic My Dyes pack tests: the write side of custom.json, round-tripped
# through the real palette parser so a special field the writer forgets fails
# a test instead of flattening a player's dye.
add_executable(MyDyesTests tests/test_mydyes.cpp src/MyDyes.cpp src/DyePalette.cpp src/JsonCodec.cpp)
target_compile_features(MyDyesTests PRIVATE cxx_std_23)
target_include_directories(MyDyesTests PRIVATE src)
target_link_libraries(MyDyesTests PRIVATE JsonCpp::JsonCpp)

# Pure-logic preview grid tests (columns, paging, disk keys, prune and LRU
# selection, the manifest codec). No engine, no D3D, no FUCK: EditorUI.cpp and
# the preview .cpps are not compiled by any test, so every decision that CAN
# live apart from them does, the DyeKey.h precedent.
# Pure-logic preview scope tests (per-path hide/show fixups and the disk-key
# tag; no engine, no D3D, no NIF). Header-only under test: the loader half
# lives in the .cpp and needs a filesystem, which is not what these rules are.
# Pure-logic preset scene tests (composing a saved outfit's pieces into one
# scene, and the cache key that decides when they are recomposed).
add_executable(PresetSceneTests tests/test_presetscene.cpp)
target_compile_features(PresetSceneTests PRIVATE cxx_std_23)
target_include_directories(PresetSceneTests PRIVATE src)

add_executable(PreviewScopesTests tests/test_previewscopes.cpp)
target_compile_features(PreviewScopesTests PRIVATE cxx_std_23)
target_include_directories(PreviewScopesTests PRIVATE src)

# ⚠ BodyMorphData.cpp rides this suite rather than earning its own, because a
# new test executable adds a pass line and the build check counts them.
add_executable(PreviewGridTests tests/test_previewgrid.cpp src/BodyMorphData.cpp)
target_compile_features(PreviewGridTests PRIVATE cxx_std_23)
target_include_directories(PreviewGridTests PRIVATE src)
target_link_libraries(PreviewGridTests PRIVATE JsonCpp::JsonCpp)

# Pure-logic dye grid tests (tile and stripe selection, no engine, no ImGui).
add_executable(DyeGridTests tests/test_dyegrid.cpp src/DyeGrid.cpp)
target_compile_features(DyeGridTests PRIVATE cxx_std_23)
target_include_directories(DyeGridTests PRIVATE src)

# Pure-logic dye condition tests (the unlock rules, no engine, no ImGui).
add_executable(DyeConditionsTests tests/test_dyeconditions.cpp src/DyeConditions.cpp)
target_compile_features(DyeConditionsTests PRIVATE cxx_std_23)
target_include_directories(DyeConditionsTests PRIVATE src)
target_link_libraries(DyeConditionsTests PRIVATE JsonCpp::JsonCpp)

# Which deed names name a Skyrim misc stat and which name one of our own
# counters. Header-only, so no source beside the test: the rule is one constexpr
# function and the tests are what stop it acquiring a "helpful" trim.
add_executable(DyeStatDeedTests tests/test_dyestatdeed.cpp)
target_compile_features(DyeStatDeedTests PRIVATE cxx_std_23)
target_include_directories(DyeStatDeedTests PRIVATE src)

# Pure-logic dye unlock tests (the earned set, charge and deeds; no engine).
add_executable(DyeUnlocksTests tests/test_dyeunlocks.cpp src/DyeUnlocks.cpp)
target_compile_features(DyeUnlocksTests PRIVATE cxx_std_23)
target_include_directories(DyeUnlocksTests PRIVATE src)

# Pure-logic shared unlock tests (the account-wide file's schema and the
# add-only merge; no engine, no save, no filesystem). ⚠ Links DyeUnlocks
# because MergeInto writes through DyeUnlockSet::Add and nothing else, which is
# the invariant the suite exists to hold.
add_executable(SharedDyeUnlocksTests tests/test_shareddyeunlocks.cpp src/SharedDyeUnlocks.cpp src/DyeUnlocks.cpp)
target_compile_features(SharedDyeUnlocksTests PRIVATE cxx_std_23)
target_include_directories(SharedDyeUnlocksTests PRIVATE src)
target_link_libraries(SharedDyeUnlocksTests PRIVATE JsonCpp::JsonCpp)

# Pure-logic dye history tests (the hand-picked colour list; no engine).
add_executable(DyeHistoryTests tests/test_dyehistory.cpp src/DyeHistory.cpp src/DyePalette.cpp src/JsonCodec.cpp)
target_compile_features(DyeHistoryTests PRIVATE cxx_std_23)
target_include_directories(DyeHistoryTests PRIVATE src)
target_link_libraries(DyeHistoryTests PRIVATE JsonCpp::JsonCpp)

# Pure-logic dye rule tests (tier and override resolution, no engine).
add_executable(DyeRulesTests tests/test_dyerules.cpp src/DyeRules.cpp src/DyeConditions.cpp)
target_compile_features(DyeRulesTests PRIVATE cxx_std_23)
target_include_directories(DyeRulesTests PRIVATE src)
target_link_libraries(DyeRulesTests PRIVATE JsonCpp::JsonCpp)

# Pure-logic requirement tests (what a locked swatch says it takes, and what
# the character reads against it; no engine, no ImGui).
add_executable(DyeRequirementsTests tests/test_dyerequirements.cpp src/DyeRequirements.cpp src/DyeConditions.cpp)
target_compile_features(DyeRequirementsTests PRIVATE cxx_std_23)
target_include_directories(DyeRequirementsTests PRIVATE src)
target_link_libraries(DyeRequirementsTests PRIVATE JsonCpp::JsonCpp)

# Pure-logic promotion tests (what a character newly earns, no engine).
# Promotion sits on top of every other pure dye module, so this is the only
# suite that links all of them together, and the only one that can run the
# SHIPPED dist/ files through the real DyeRules::Load and DyePalette::Load and
# assert what a character actually earns off them. That is why the palette is
# in this target's sources: nothing else holds the rules and the colours at the
# same time, which is the same reason the free-dye reports live in promotion.
add_executable(DyePromotionTests tests/test_dyepromotion.cpp src/DyePromotion.cpp src/DyeRules.cpp src/DyeConditions.cpp src/DyeUnlocks.cpp src/DyePalette.cpp src/JsonCodec.cpp)
target_compile_features(DyePromotionTests PRIVATE cxx_std_23)
target_include_directories(DyePromotionTests PRIVATE src)
target_link_libraries(DyePromotionTests PRIVATE JsonCpp::JsonCpp)
# Where the shipped files are, so the test finds them whatever the working
# directory is. Not derived from the working directory on purpose: the suite
# moves it, and a path that resolved differently under the build script than
# under a bare run is a test that quietly checks nothing.
target_compile_definitions(DyePromotionTests PRIVATE
    FR_DIST_DIR="${CMAKE_SOURCE_DIR}/dist/SKSE/Plugins/FittingRoom")

# The unlock card's timing and stacking, with no engine and no clock: time is a
# parameter, so a burst of forty colours across six minutes of dwell runs in a
# microsecond. The awkward cases are the point - a batch bigger than the screen,
# a card retiring while another is still arriving, the load boundary landing
# mid-stack, and a clock that goes backwards across a load screen.
add_executable(DyeCardQueueTests tests/test_dyecardqueue.cpp src/DyeCardQueue.cpp)
target_compile_features(DyeCardQueueTests PRIVATE cxx_std_23)
target_include_directories(DyeCardQueueTests PRIVATE src)

# Pure-logic set-detection tests.
add_executable(SetDetectorTests tests/test_setdetector.cpp src/SetDetector.cpp)
target_compile_features(SetDetectorTests PRIVATE cxx_std_23)
target_include_directories(SetDetectorTests PRIVATE src)

# Pure-logic Shape overlay tests: what the page pushes at RaceMenu's node
# transforms and what it reads back. The engine half (NodeTransformApi) needs
# skee and is field-verified; this is the half that can be wrong quietly.
add_executable(ShapeOverlayTests tests/test_shapeoverlay.cpp)
target_compile_features(ShapeOverlayTests PRIVATE cxx_std_23)
target_include_directories(ShapeOverlayTests PRIVATE src)

# The Overlays page's engine-free half: node names, skee's override key numbers,
# and the index rule that decides whether a read finds what a write stored.
#
# ⚠ THESE ARE OTHER PEOPLE'S CONSTANTS AND THE FAULTS ARE ALL SILENT. A node
# name built with %d instead of {} misses every lookup without logging, and a
# tint read at index 0 rather than 0xFF comes back empty while the colour is
# plainly on the character. Neither shows up as a crash or a warning, so the
# numbers are pinned here rather than trusted to review.
add_executable(OverlayPlanTests tests/test_overlayplan.cpp)
target_compile_features(OverlayPlanTests PRIVATE cxx_std_23)
target_include_directories(OverlayPlanTests PRIVATE src)

# The skin changer's engine-free half: a pack is a folder of replacer files
# matched to a shape by FILE NAME, the node-name rule skee's armour overrides
# impose at attach ("" for a one-geometry addon), and the 'SKIN' record that
# remembers every override this mod wrote so Default can take exactly those off.
#
# ⚠ THE NAME RULE IS ANOTHER PLUGIN'S CONSTANT AND ITS FAULT IS SILENT: an
# override stored under "artHands" is never looked up on a one-shape addon and
# nothing logs the miss, so the rule is pinned here rather than trusted.
add_executable(SkinPlanTests tests/test_skinplan.cpp)
target_compile_features(SkinPlanTests PRIVATE cxx_std_23)
target_include_directories(SkinPlanTests PRIVATE src)

# The overlay offset sliders' engine-free half (OS-209): the transform is
# quantised, hashed with its source into a file stem, written beside a sidecar
# that decodes it, and baked under a DX10 DDS header. The one mapping the
# compute shader runs is written here a second time and inverted, so the tests
# hold the shader's arithmetic without a GPU.
#
# ⚠ THE FILE STEM IS THE CACHE KEY AND EVERY BAKE ON DISK IS NAMED BY IT. A
# key that drifted, or a header whose byte was in the wrong place, would be
# served silently for as long as the file lived, so both are pinned.
add_executable(OverlayTransformTests tests/test_overlaytransform.cpp)
target_compile_features(OverlayTransformTests PRIVATE cxx_std_23)
target_include_directories(OverlayTransformTests PRIVATE src)

# Which overlay art belongs on which location, and the three tiers that decide:
# the pack's own RaceMenu registration, then a location word in the name or the
# folder, then every location.
#
# ⚠⚠ THE THIRD TIER IS WHAT THIS SUITE IS REALLY FOR. RaceMenu's lists are
# authored per pack in Papyrus, so nothing can place art whose pack ships no
# script, and a rule that answered kNone there would empty the picker for two
# thirds of the installed library without logging a thing.
#
# It also runs the SHIPPED table through the real reader, which is the only
# check anywhere that the PowerShell generator and this loader still agree about
# the key shape. Same FR_DIST_DIR argument as DyePromotionTests: the path is
# handed in at configure time so the suite runs identically under the build
# script and under a bare run.
add_executable(OverlayLocationsTests tests/test_overlaylocations.cpp src/OverlayLocations.cpp)
target_compile_features(OverlayLocationsTests PRIVATE cxx_std_23)
target_include_directories(OverlayLocationsTests PRIVATE src)
target_link_libraries(OverlayLocationsTests PRIVATE JsonCpp::JsonCpp)
target_compile_definitions(OverlayLocationsTests PRIVATE
    FR_DIST_DIR="${CMAKE_SOURCE_DIR}/dist/SKSE/Plugins/FittingRoom")

# Makeup's vocabulary and its packing: the tint mask layers, which are a
# different system from the [Ovl] overlay nodes above and not the same page with
# different art.
#
# ⚠⚠ WHAT THIS SUITE REALLY GUARDS IS THE MEANING OF "IN USE". Every tint layer
# carries a texture at all times, because the list is built from the race and a
# race gives every layer default art, so an unused layer is one at zero STRENGTH
# rather than one with no texture. Reading it the overlays way reports every
# character as wearing all fifteen layers and the page's own count never falls.
#
# The packed colours it checks are measured values out of the installed presets
# rather than invented ones, which is what makes a round trip failure mean
# something.
add_executable(MakeupPlanTests tests/test_makeupplan.cpp)
target_compile_features(MakeupPlanTests PRIVATE cxx_std_23)
target_include_directories(MakeupPlanTests PRIVATE src)

# Pure-logic reading of the installed body's slider vocabulary out of
# BodySlide's own files. Deliberately no XML library: some shipped category
# files are not well-formed and a conforming parser drops them whole.
add_executable(BodyMorphCatalogTests tests/test_bodymorphcatalog.cpp)
target_compile_features(BodyMorphCatalogTests PRIVATE cxx_std_23)
target_include_directories(BodyMorphCatalogTests PRIVATE src)

# Pure-logic reading of the TRIP file BodySlide writes beside a built mesh,
# which is what says whether a slider can move this character at all. The
# slider set only says what the set could build, and the two came apart in the
# field: a preset naming a set nobody has installed.
add_executable(BodyMorphTriTests tests/test_bodymorphtri.cpp)
target_compile_features(BodyMorphTriTests PRIVATE cxx_std_23)
target_include_directories(BodyMorphTriTests PRIVATE src)

# Pure-logic tests for which pages the player wants in the rail. The empty
# case is the one worth having a suite for: every page off is reachable by
# hand-editing the INI whether or not the panel allows it, so it has to be
# answered rather than assumed away.
add_executable(PagePolicyTests tests/test_page_policy.cpp)
target_compile_features(PagePolicyTests PRIVATE cxx_std_23)
target_include_directories(PagePolicyTests PRIVATE src)

# Pure-logic editor-gate tests.
add_executable(EditorGateTests tests/test_editorgate.cpp)
target_compile_features(EditorGateTests PRIVATE cxx_std_23)
target_include_directories(EditorGateTests PRIVATE src)

# Pure-logic tests for the one list of menus the editor may be hosted by. The
# list was six copies of the same two lines before it was a function, and the
# two ways they can disagree are an editor that shuts itself the frame it opens
# and a world camera nailed to a follower during gameplay. HostGuard.h keeps the
# menu names as literals so this can run without a game; HostGuard.cpp
# static_asserts them against RE's own constants.
add_executable(HostGuardTests tests/test_hostguard.cpp)
target_compile_features(HostGuardTests PRIVATE cxx_std_23)
target_include_directories(HostGuardTests PRIVATE src)

# Pure-logic tests for the ControlMap array-shape probe. The engine half of
# TextEntry.cpp cannot be reached without a running game; the predicate that
# decides which candidate base is the real one can, and it is the part where
# being wrong reads as healthy.
add_executable(TextEntryTests tests/test_textentry.cpp)
target_compile_features(TextEntryTests PRIVATE cxx_std_23)
target_include_directories(TextEntryTests PRIVATE src)

# Pure-logic onboarding-tutorial tests. The fire-once decision lives outside
# EditorUI.cpp precisely so it can be covered here.
add_executable(TutorialPlanTests tests/test_tutorialplan.cpp)
target_compile_features(TutorialPlanTests PRIVATE cxx_std_23)
target_include_directories(TutorialPlanTests PRIVATE src)

# Pure-logic keyboard-arbiter tests (OS-46 doubled first keystroke).
add_executable(KeyboardArbiterTests tests/test_keyboardarbiter.cpp)
target_compile_features(KeyboardArbiterTests PRIVATE cxx_std_23)
target_include_directories(KeyboardArbiterTests PRIVATE src)

# Pure-logic weapon-class mapping tests (weapon + quiver transmog, stage 1).
add_executable(WeaponSlotsTests tests/test_weaponslots.cpp)
target_compile_features(WeaponSlotsTests PRIVATE cxx_std_23)
target_include_directories(WeaponSlotsTests PRIVATE src)

# Pure-logic NPC render-decision tests (worn-required rule, snapshot source
# precedence - the pure core of the OutfitSession actor dimension).
add_executable(NpcSessionTests tests/test_npcsession.cpp)
target_compile_features(NpcSessionTests PRIVATE cxx_std_23)
target_include_directories(NpcSessionTests PRIVATE src)

# Pure-logic follower hair-style reconcile (what the load-time push decides).
add_executable(NpcHairPlanTests tests/test_npchairplan.cpp)
target_compile_features(NpcHairPlanTests PRIVATE cxx_std_23)
target_include_directories(NpcHairPlanTests PRIVATE src)

# Pure-logic head-part offer rule (which parts a given actor is shown, for
# hair, eyes and brows alike). The lookup around it needs TESDataHandler and
# cannot be tested; this is the half that can be wrong.
add_executable(HeadPartPlanTests tests/test_headpartplan.cpp)
target_compile_features(HeadPartPlanTests PRIVATE cxx_std_23)
target_include_directories(HeadPartPlanTests PRIVATE src)

# Pure-logic rules for head-part slots the load order invented (horns, ears).
# The discovery walk needs TESDataHandler; what can be wrong is the label
# ladder, which scavenges a name from a convention and has to fail into the
# next arm rather than into an empty row.
add_executable(HeadPartSlotPlanTests tests/test_headpartslotplan.cpp)
target_compile_features(HeadPartSlotPlanTests PRIVATE cxx_std_23)
target_include_directories(HeadPartSlotPlanTests PRIVATE src)

# What the camera frames while a head part is browsed. The case that earns the
# suite is the DISCOVERED slot: a horn is not a HeadPart::Kind and never can be,
# so a Kind-only reader answers "nothing" and the caller falls through to the
# armour branch, whose default bit is the body. That is a camera on the torso
# while the player is fitting horns (user 2026-08-19).
add_executable(HeadShotPlanTests tests/test_headshotplan.cpp)
target_compile_features(HeadShotPlanTests PRIVATE cxx_std_23)
target_include_directories(HeadShotPlanTests PRIVATE src)

# Pure-logic head-part PRECEDENCE (outfit, then character default, then their
# own). The rung-usability half is what stops a stored default being applied to
# a character whose race or sex would never be offered it.
add_executable(HeadPartLadderTests tests/test_headpartladder.cpp)
target_compile_features(HeadPartLadderTests PRIVATE cxx_std_23)
target_include_directories(HeadPartLadderTests PRIVATE src)

# What the dismember sweep MEASURED about head-part occupants, which is the
# only thing standing between an invisible helmet and a bald character. The
# case that earns the suite is r30's: a look apply rides ChangeRace out to the
# vanilla race and back, the helmet that stages nothing on the custom race
# really draws on the vanilla one, and a verdict keyed on the actor alone was
# zeroed by that honest reading every single apply.
add_executable(MeasuredGhostsTests tests/test_measuredghosts.cpp)
target_compile_features(MeasuredGhostsTests PRIVATE cxx_std_23)
target_include_directories(MeasuredGhostsTests PRIVATE src)

# Pure-logic FSMP build-fingerprint table (the offline half of the bridge;
# the engine half - module lookup, the SkinSingleGeometry call - is
# field-verified separately, see docs/re/fsmp-cooperative-hair.md).
add_executable(FsmpBridgeTests tests/test_fsmpbridge.cpp)
target_compile_features(FsmpBridgeTests PRIVATE cxx_std_23)
target_include_directories(FsmpBridgeTests PRIVATE src)

# Pure-logic player refresh cooldown around blocking transitions.
add_executable(RefreshGateTests tests/test_refreshgate.cpp)
target_compile_features(RefreshGateTests PRIVATE cxx_std_23)
target_include_directories(RefreshGateTests PRIVATE src)

# Pure-logic dye-stripe flash lifetime (OS-144): when a lit stripe has to be
# put back, and which shapes a stripe owns. The scenegraph write is engine-side;
# what is testable is the part where forgetting a case leaves a shape glowing.
add_executable(DyeFlashTests tests/test_dyeflash.cpp)
target_compile_features(DyeFlashTests PRIVATE cxx_std_23)
target_include_directories(DyeFlashTests PRIVATE src)

# Pure-logic requip transition timing (OS-206): the curves the flourish runs on
# and the phase machine around them. The scenegraph write is engine-side; what
# is testable is that Burn reaches exactly zero alpha and that Wait never
# advances on a clock.
add_executable(RequipFlourishTests tests/test_requipflourish.cpp)
target_compile_features(RequipFlourishTests PRIVATE cxx_std_23)
target_include_directories(RequipFlourishTests PRIVATE src)

# Pure-logic requip slot diff (OS-206): which armour bits a swap actually
# changes. The subtlety worth a test is that a passthrough slot keeps stale
# style data, so comparing the struct wholesale arms slots nobody touched.
add_executable(RequipDiffTests tests/test_requipdiff.cpp)
target_compile_features(RequipDiffTests PRIVATE cxx_std_23)
target_include_directories(RequipDiffTests PRIVATE src)

# Pure-logic hover dye preview (spec 2026-08-09): the debounce a hovered swatch
# runs, and which node walk an armed preview substitutes into. The consult
# lives in OutfitDye.cpp and the hover in EditorUI.cpp, neither of which any
# test compiles.
add_executable(DyePreviewTests tests/test_dyepreview.cpp)
target_compile_features(DyePreviewTests PRIVATE cxx_std_23)
target_include_directories(DyePreviewTests PRIVATE src)

# Pure-logic chamfer geometry (2026-08-12), ported from Menu Studio with the
# drawing layer left behind. The fill tiles an octagon into three quads by hand,
# and a gap or an overlap between them looks correct on an opaque fill and shows
# as a seam on a translucent one, which is exactly what no screenshot review
# catches reliably. The area identity does.
add_executable(ChamferPolicyTests tests/test_chamferpolicy.cpp)
target_compile_features(ChamferPolicyTests PRIVATE cxx_std_23)
target_include_directories(ChamferPolicyTests PRIVATE src)

# The undo/redo chord table (2026-08-18). Four pages draw undo and redo buttons
# and only the outfit page answered the keyboard, so the bindings moved into one
# pure header the pages share. Worth pinning because Z appears in BOTH chords:
# an implementation that tests it before asking about shift turns Ctrl+Shift+Z
# into a second undo, which walks the player the wrong way down their own
# history and looks like a dropped keypress rather than a wrong branch.
add_executable(UndoRedoPlanTests tests/test_undoredoplan.cpp)
target_compile_features(UndoRedoPlanTests PRIVATE cxx_std_23)
target_include_directories(UndoRedoPlanTests PRIVATE src)

# Pure-logic layout for the hand-rolled outfit tab strip (2026-08-12). The
# strip stopped being FUCK::BeginTabItem because a FLICK tab submits its
# background and its label in one call and nothing of ours can land behind the
# text, so the widths, the running x and the horizontal scroll are all ours to
# get right. The scroll is the part worth pinning: the FUCK ABI has no
# SetScrollX at all, so it is arithmetic here rather than a call, and a strip
# that re-scrolls every frame fights the wheel.
add_executable(OutfitTabStripTests tests/test_outfittabstrip.cpp)
target_compile_features(OutfitTabStripTests PRIVATE cxx_std_23)
target_include_directories(OutfitTabStripTests PRIVATE src)

# Whether a stored fUiScale is a preference or a leftover. Save writes that key,
# so every install carries the default of the day it was made and no later
# default can reach it; the only thing separating a July 0.8 from a deliberate
# one is a version stamp. The rule can be wrong in both directions, which is
# what earns it an executable.
add_executable(UiScaleMigrationTests tests/test_uiscalemigration.cpp)
target_compile_features(UiScaleMigrationTests PRIVATE cxx_std_23)
target_include_directories(UiScaleMigrationTests PRIVATE src)

# Pure-logic hover dwell (item 1, 2026-08-12): the "you have looked at this"
# edge that clears a gold mark, over DyePreview::Decide's transition. The sweep
# case is the one that matters and no test compiles EditorUI.cpp.
add_executable(DwellAckTests tests/test_dwellack.cpp)
target_compile_features(DwellAckTests PRIVATE cxx_std_23)
target_include_directories(DwellAckTests PRIVATE src)

# Pure-logic face-settle wait (the 2026-08-22 page-save freeze): the paused
# editor spends no budget, the unpaused budget is wall-clock, and a landed
# answer-plus-file wins on any slice. The freeze state itself is the first
# case.
add_executable(FaceWaitTests tests/test_facewait.cpp)
target_compile_features(FaceWaitTests PRIVATE cxx_std_23)
target_include_directories(FaceWaitTests PRIVATE src)

# Pure-logic Apparel Preview wire format (OS-145): what each of the two
# messages does to the shared word, and the compatibility pins that say what an
# OLDER Fitting Room would make of a NEWER Apparel Preview's payload.
add_executable(ApparelPreviewWireTests tests/test_apparelpreviewwire.cpp)
target_compile_features(ApparelPreviewWireTests PRIVATE cxx_std_23)
target_include_directories(ApparelPreviewWireTests PRIVATE src)

# Pure-logic dye repaint gate: staged-but-uncloned vs nothing worn, and the
# bounded deferred-attempt budget behind it.
add_executable(DyeGateTests tests/test_dyegate.cpp)
target_compile_features(DyeGateTests PRIVATE cxx_std_23)
target_include_directories(DyeGateTests PRIVATE src)

# Pure-logic OS-140 arithmetic: which source mip a capped build reads, what a
# mip chain costs, and when a colour has settled enough to be worth a full one.
add_executable(DyeQualityTests tests/test_dyequality.cpp)
target_compile_features(DyeQualityTests PRIVATE cxx_std_23)
target_include_directories(DyeQualityTests PRIVATE src)

# Pure-logic two-stop ramp arithmetic: where the second stop sits at a given
# strength, which mode a shape actually gets once its feature is known, and
# which single target the ramp lands on. Engine-free because the two callers
# that consume it live in OutfitDye.cpp and DyeTexture.cpp, neither of which any
# test compiles, so a decision left in either would reach the field unexercised.
add_executable(DyeRampTests tests/test_dyeramp.cpp)
target_compile_features(DyeRampTests PRIVATE cxx_std_23)
target_include_directories(DyeRampTests PRIVATE src)

# Which arithmetic a DYE asks for, as a stored byte. Pure and tested for the
# same reason the ramp beside it is, plus one this one owns alone: the zero of
# this enum DEFERS to the install's setting rather than naming a curve, and that
# is the whole reason a record written before the feature renders unchanged. A
# zero that quietly became soft light would restyle every dye in every save, and
# the only place that decision is exercised is here.
add_executable(DyeBlendTests tests/test_dyeblend.cpp)
target_compile_features(DyeBlendTests PRIVATE cxx_std_23)
target_include_directories(DyeBlendTests PRIVATE src)

# The RAMP half of a tinted texture's cache key. Pure and tested because the
# fault it prevents is invisible: two dyes sharing a primary hex and differing
# only in their second stop would collide in DyeTexture's cache and the second
# would render as the first, silently and for as long as the entry lives.
# DyeTexture.cpp is not compiled by any test, so the arithmetic lives apart.
add_executable(DyeKeyTests tests/test_dyekey.cpp)
target_compile_features(DyeKeyTests PRIVATE cxx_std_23)
target_include_directories(DyeKeyTests PRIVATE src)

# Pure-logic dye strength blend: how much of a chosen colour lands, as a walk
# from that colour toward the shader's neutral. Engine-free so the paint path
# and the slider cannot disagree about the arithmetic.
add_executable(DyeStrengthTests tests/test_dyestrength.cpp)
target_compile_features(DyeStrengthTests PRIVATE cxx_std_23)
target_include_directories(DyeStrengthTests PRIVATE src)

# A pearl on a Community Shaders True PBR material: which features the dye may
# write, the arithmetic of each write, and the hand-walked MSVC RTTI gate that
# keeps a raw offset store off the wrong allocation. The offsets and flag bits
# are pinned here because they were MEASURED out of the shipped CS binary, and
# an edit to them should fail a test rather than a player.
add_executable(PbrPearlTests tests/test_pbrpearl.cpp)
target_compile_features(PbrPearlTests PRIVATE cxx_std_23)
target_include_directories(PbrPearlTests PRIVATE src)

# The Seamstone's charge arithmetic (OS-129). Off-engine on purpose: the charge
# is per-save state, and this is the half that can be settled before any of it
# reaches a co-save.
add_executable(SeamstoneChargeTests tests/test_seamstonecharge.cpp)
target_compile_features(SeamstoneChargeTests PRIVATE cxx_std_23)
target_include_directories(SeamstoneChargeTests PRIVATE src)

# Real ImGui tab-state regression test for the Presets source tabs.
add_executable(ShowcaseTabsTests tests/test_showcase_tabs.cpp)
target_compile_features(ShowcaseTabsTests PRIVATE cxx_std_23)
target_include_directories(ShowcaseTabsTests PRIVATE src)
target_link_libraries(ShowcaseTabsTests PRIVATE imgui::imgui)

# The Presets hover, which IS the detail column the one-pane rework deleted
# (2026-08-12). Two things need holding: a Discovered preset's file is the
# literal placeholder "discovered" and printing it alone was the whole tooltip,
# and everything the deleted column carried has to keep coming out of this
# function or it is gone with no screen left to notice on.
add_executable(PresetTooltipTests tests/test_presettooltip.cpp)
target_compile_features(PresetTooltipTests PRIVATE cxx_std_23)
target_include_directories(PresetTooltipTests PRIVATE src)

# Pure-logic rules data-model tests (Compose + condition matching).
add_executable(RuleModelTests tests/test_rulemodel.cpp)
target_compile_features(RuleModelTests PRIVATE cxx_std_23)
target_include_directories(RuleModelTests PRIVATE src)

# Pure-logic rules decision-core tests (precedence, dwell, pin).
add_executable(RuleEngineTests tests/test_ruleengine.cpp src/RuleEngine.cpp)
target_compile_features(RuleEngineTests PRIVATE cxx_std_23)
target_include_directories(RuleEngineTests PRIVATE src)

# Rules JSON codec tests (pure logic, needs jsoncpp).
add_executable(RuleCodecTests tests/test_rulecodec.cpp src/RuleCodec.cpp)
target_compile_features(RuleCodecTests PRIVATE cxx_std_23)
target_include_directories(RuleCodecTests PRIVATE src)
target_link_libraries(RuleCodecTests PRIVATE JsonCpp::JsonCpp)

# Pure-logic Advanced-condition clause tokenizer/shape-checker tests (Task
# 8b). No RE:: - the RE::-dependent half (AdvancedCondition.cpp: table
# lookup, form resolution, TESCondition construction) is untestable offline
# and intentionally excluded from every test target, same as always.
add_executable(AdvancedConditionParseTests tests/test_advancedconditionparse.cpp)
target_compile_features(AdvancedConditionParseTests PRIVATE cxx_std_23)
target_include_directories(AdvancedConditionParseTests PRIVATE src)

add_executable(DirectEntryTests tests/test_directentry.cpp)
target_compile_features(DirectEntryTests PRIVATE cxx_std_23)
target_include_directories(DirectEntryTests PRIVATE src)

if(NOT DEFINED OUTPUT_FOLDER)
    # ⚠⚠ THE DEFAULT IS A QUARANTINE, AND IT MUST STAY ONE. It used to be
    # C:/Games/Nolvus/Instances/Nolvus Awakening/MODS/mods/Fitting Room, the
    # shared base mod that carries the esp, the dye packs and the unlock rules
    # and deliberately carries no DLL, so every slot above it wins the plugin.
    # A build that lands there puts a DLL in the base mod and runs the POST_BUILD
    # copy_directory of dist over the very folder the shared Dyes and Unlocks
    # live in.
    #
    # That is not hypothetical. On 2026-07-29 a build from a checkout nobody
    # thought was live deployed a two-week-old DLL and translation file over the
    # live folder, and it read in game as a code regression: raw $FR_ keys,
    # deleted UI back, both crash fixes gone.
    #
    # tools\build.bat passes OUTPUT_FOLDER explicitly on every configure, so this
    # default only governs a hand-run cmake. Pointing it at the game meant that
    # hand-run cmake was the loaded gun. Deploying to the game is redeploy.ps1's
    # job, it stages into a per-branch MO2 slot, and it is the only thing that
    # should ever write into that instance.
    set(OUTPUT_FOLDER "$ENV{TEMP}/fittingroom-unrouted-build")
endif()
set(DLL_FOLDER "${OUTPUT_FOLDER}/SKSE/Plugins")
message(STATUS "FittingRoom deploy folder: ${DLL_FOLDER}")
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${DLL_FOLDER}"
    # Deploy the FLICK translation file first, so it lands even when the game is
    # running (only the DLL is locked). Path/name match FUCK::Connect("Fitting Room").
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${OUTPUT_FOLDER}/Interface/Translations"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_SOURCE_DIR}/dist/Interface/Translations/Fitting Room_ENGLISH.txt" "${OUTPUT_FOLDER}/Interface/Translations/Fitting Room_ENGLISH.txt"
    # ⚠ THE WHOLE FittingRoom DIRECTORY, NOT A LIST OF ITS SUBDIRECTORIES.
    # Plain data the plugin reads at kDataLoaded, not linked into the DLL, so
    # it needs its own copy rule.
    #
    # This was two lines naming Dyes and Unlocks, and the second one existed
    # only because the first was hard coded to Dyes and picked up no sibling.
    # Its comment said "any dist/ subdirectory added later needs its own line
    # here too", which describes the failure mode accurately and then preserves
    # it: Presets/ and icons.ttf were already siblings and already reached
    # build/deploy nowhere. Enumerating a directory listing in a build file is
    # a rule that has to be maintained by remembering, and the last thing this
    # branch needs is another way for the deployed data to be a subset of the
    # shipped data.
    #
    # The consequence when the pair comes apart is worth restating, since it is
    # what made the second line matter. Every colour in the pack carries a
    # rarity, a rarity that finds no tier resolves to no rule, and no rule
    # means no requirement: a deploy that got the palette and missed the rules
    # hands out all 318 colours at level 1 and writes them permanently into
    # that character's co-save, because unlocks are add only.
    #
    # copy_directory rather than copy_if_different on named files, so anything
    # dropped into dist/ is picked up with no CMakeLists change;
    # copy_directory_if_different needs CMake 3.26 and this project's floor is
    # 3.21, so this always rewrites rather than skipping unchanged files, which
    # is cheap for a folder of small JSON files and one font.
    #
    # tools/make_fomod.sh does NOT read this folder. It stages the release
    # straight out of dist/ with one recursive copy of the same directory, so
    # the FOMOD already shipped Presets and icons.ttf while build/deploy did
    # not; this brings the two into line rather than changing what ships.
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_SOURCE_DIR}/dist/SKSE/Plugins/FittingRoom" "${OUTPUT_FOLDER}/SKSE/Plugins/${FR_DATA_SUBDIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>" "${DLL_FOLDER}/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
    VERBATIM)

if(FR_BODY_STUDIO_DEV)
    # The development INI lives inside the build-qualified data root. Copy it
    # after the shared read-only assets so no release INI is ever deployed or
    # consulted by this channel.
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${CMAKE_SOURCE_DIR}/dist/dev/body-studio/FittingRoom.ini"
                "${OUTPUT_FOLDER}/SKSE/Plugins/${FR_DATA_SUBDIR}/FittingRoom.ini"
        VERBATIM)
endif()

# ⚠ EVERY *Tests EXECUTABLE RESERVES 8 MB OF STACK, and this is not a
# precaution. sizeof(OS::Outfit) is 7536 bytes as of 2026-08-03 and tests/
# test_outfit.cpp is one ~2000-line main() holding dozens of Outfit locals, so
# that frame is already close to the default 1 MB reserve. It went over during
# the weapon dye work: a first cut storing weapon colours as three flat arrays
# took Outfit to 11760 bytes and OutfitTests died with 0xC00000FD,
# STACK_OVERFLOW, before running a single CHECK. Sparse storage brought Outfit
# back down, but the margin was never comfortable and the next struct to grow
# should not have to rediscover this.
#
# ⚠ It presented as a suite with NO output and NO failures. tools/
# build_tests.bat's header already records that shape: a suite that CRASHES
# rather than returning 1 is only caught by "%errorlevel% neq 0", never by
# "if errorlevel 1", because that is a SIGNED >= test and every NTSTATUS crash
# code is negative as an int32. Both scripts use the correct form. Do not relax
# either, and do not treat this stack bump as a substitute for that check.
#
# Discovered by name rather than listed, deliberately. A hand-maintained list of
# test targets is exactly what drifted between build.bat and build_tests.bat and
# once reported eight suites green without building them.
get_property(_fr_all_targets DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
             PROPERTY BUILDSYSTEM_TARGETS)
foreach(_fr_target IN LISTS _fr_all_targets)
    if(_fr_target MATCHES "Tests$")
        target_link_options(${_fr_target} PRIVATE /STACK:8388608)
    endif()
endforeach()
