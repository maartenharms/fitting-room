# RaceMenu modder interface

`IPluginInterface.h` is the public modder-resource header shipped with RaceMenu
Special Edition 0.4.19.16 (the Skyrim 1.6 build installed for the Body Studio
ownership proof), with trailing whitespace normalized. It is vendored so
Fitting Room can compile against `IBodyMorphInterface` without linking to
RaceMenu.

Upstream project: https://github.com/expired6978/SKSE64Plugins

The header is included by `RaceMenuMorphApi.cpp` only. No RaceMenu type crosses
that translation unit's public boundary.
