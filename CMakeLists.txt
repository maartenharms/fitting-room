cmake_minimum_required(VERSION 3.21)

# "Fitting Room" (formerly Outfit Slots) - ESO-style transmog with saved outfit slots.
# Skyrim SE 1.5.97 only. DLL name = FittingRoom.dll (project name drives it).
project(FittingRoom VERSION 0.2.1 LANGUAGES CXX)

find_package(CommonLibSSE CONFIG REQUIRED)
# jsoncpp's exported config predates CMake 3.5's policy floor.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
find_package(jsoncpp CONFIG REQUIRED)
unset(CMAKE_POLICY_VERSION_MINIMUM)
find_package(imgui CONFIG REQUIRED)

add_commonlibsse_plugin(${PROJECT_NAME}
    SOURCES
        src/PCH.h
        src/plugin.cpp
        src/VersionCheck.h
        src/VersionCheck.cpp
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
        src/SamCompat.h
        src/SamCompat.cpp
        src/CrashGuard.h
        src/CrashGuard.cpp
        src/Favorites.h
        src/Favorites.cpp
        src/RecentMods.h
        src/RecentMods.cpp
        src/Icons.h
        src/Diagnostics.h
        src/Diagnostics.cpp
        src/SceneGuard.h
        src/SceneGuard.cpp
        src/RaceSwitchSink.h
        src/RaceSwitchSink.cpp
        src/Migration.h
        src/OutfitSession.h
        src/OutfitSession.cpp
        src/BipedHooks.h
        src/BipedHooks.cpp
        src/BipedPost.h
        src/BipedPost.cpp
        src/WeaponSlots.h
        src/WeaponHooks.h
        src/WeaponHooks.cpp
        src/InputListener.h
        src/InputListener.cpp
        src/MenuButton.h
        src/MenuButton.cpp
        src/LoreModule.h
        src/LoreModule.cpp
        src/KeyboardArbiter.h
        src/ImGuiOverlay.h
        src/ImGuiOverlay.cpp
        src/EditorUI.h
        src/EditorUI.cpp
        src/FuckCompat.h
        src/EditorWindow.h
        src/EditorWindow.cpp
        src/EditorStyle.h
        src/EditorStyle.cpp
        src/JsonCodec.h
        src/JsonCodec.cpp
        src/PresetStore.h
        src/PresetStore.cpp
        src/Persistence.h
        src/Persistence.cpp
        src/Collection.h
        src/Collection.cpp
        src/Settings.h
        src/Settings.cpp
        src/SettingsUI.h
        src/SettingsUI.cpp
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)
# extern/ holds the vendored FUCK API header (FUCK_API.h, soft dependency,
# resolved at runtime via RequestFUCK / GetProcAddress - no link-time coupling).
target_include_directories(${PROJECT_NAME} PRIVATE extern)
target_link_libraries(${PROJECT_NAME} PRIVATE JsonCpp::JsonCpp imgui::imgui d3d11 dxgi)
target_compile_definitions(${PROJECT_NAME} PRIVATE IMGUI_DEFINE_MATH_OPERATORS)

# Pure-logic unit tests (no RE/engine deps).
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

# Pure-logic set-detection tests.
add_executable(SetDetectorTests tests/test_setdetector.cpp src/SetDetector.cpp)
target_compile_features(SetDetectorTests PRIVATE cxx_std_23)
target_include_directories(SetDetectorTests PRIVATE src)

# Pure-logic editor-gate tests.
add_executable(EditorGateTests tests/test_editorgate.cpp)
target_compile_features(EditorGateTests PRIVATE cxx_std_23)
target_include_directories(EditorGateTests PRIVATE src)

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

if(NOT DEFINED OUTPUT_FOLDER)
    set(OUTPUT_FOLDER "C:/Games/Nolvus/Instances/Nolvus Awakening/MODS/mods/Fitting Room")
endif()
set(DLL_FOLDER "${OUTPUT_FOLDER}/SKSE/Plugins")
message(STATUS "FittingRoom deploy folder: ${DLL_FOLDER}")
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${DLL_FOLDER}"
    # Deploy the FLICK translation file first, so it lands even when the game is
    # running (only the DLL is locked). Path/name match FUCK::Connect("Fitting Room").
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${OUTPUT_FOLDER}/Interface/Translations"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_SOURCE_DIR}/dist/Interface/Translations/Fitting Room_ENGLISH.txt" "${OUTPUT_FOLDER}/Interface/Translations/Fitting Room_ENGLISH.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>" "${DLL_FOLDER}/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
    VERBATIM)
