set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

# Release-only: we ship a Release DLL. Halves dependency build time and avoids
# building debug variants of the vendored libraries.
set(VCPKG_BUILD_TYPE release)
