# Fitting Room

ESO-style transmog for Skyrim Special Edition and Anniversary Edition. Save
outfits and wear them over your real gear, so the screen shows the look you
picked while the game keeps scoring the armour you actually have on.

https://www.nexusmods.com/skyrimspecialedition/mods/185342

## Requirements

* [CMake](https://cmake.org/)
	* Add this to your `PATH`
* [Vcpkg](https://github.com/microsoft/vcpkg)
	* Add the environment variable `VCPKG_ROOT` with the value as the path to the folder containing vcpkg
* [Visual Studio Community 2022](https://visualstudio.microsoft.com/)
	* Desktop development with C++

## User Requirements

* Skyrim SE 1.5.97, or AE 1.6.317 and later. VR is not supported.
* [SKSE64](https://skse.silverlock.org/) for your runtime
* [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444)
	* The SE or the AE database, whichever matches your game
* [FLICK](https://www.nexusmods.com/skyrimspecialedition/mods/181603)
	* The UI framework the editor draws on. Install FLICK's AE build on AE.
* [OBody NG](https://www.nexusmods.com/skyrimspecialedition/mods/77016) 4.4.0 or newer, optional, for per-outfit body presets

## Building

```
git clone https://github.com/maartenharms/fitting-room.git
cd fitting-room
cmake --preset release
cmake --build build/release
```

## License

[GPL-3.0](LICENSE)
