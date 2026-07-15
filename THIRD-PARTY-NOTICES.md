# Third-party notices

Fitting Room is licensed GPL-3.0 (see [LICENSE](LICENSE)). It builds on the following third-party components; each remains under its own license.

## Vendored headers (in `extern/`)

| File | Origin | License |
|---|---|---|
| `FUCK_API.h` | [FLICK](https://www.nexusmods.com/skyrimspecialedition/mods/181603) by Fuzzles | GPL-3.0 (project license; the header itself carries no separate notice) |

`FUCK_API.h` declares a C-ABI that binds at runtime against `FUCK.dll` via `GetModuleHandle`/`RequestFUCK`; no third-party object code is linked into this plugin. It references [Dear ImGui](https://github.com/ocornut/imgui) types (MIT), pulled in as a build dependency below.

## Bundled assets

| Asset | Origin | License |
|---|---|---|
| `icons.ttf` | [Font Awesome](https://fontawesome.com/) Free (the icon webfont) | SIL OFL 1.1 |

## Build dependencies (via vcpkg)

| Component | License |
|---|---|
| [CommonLibSSE-NG](https://github.com/CharmedBaryon/CommonLibSSE-NG) | MIT |
| [xbyak](https://github.com/herumi/xbyak) | BSD-3-Clause |
| [simpleini](https://github.com/brofield/simpleini) | MIT |
| [jsoncpp](https://github.com/open-source-parsers/jsoncpp) | Public Domain / MIT (dual) |
| [Dear ImGui](https://github.com/ocornut/imgui) | MIT |
| [fmt](https://github.com/fmtlib/fmt) | MIT, with binary-embedding exception |
| [spdlog](https://github.com/gabime/spdlog) | MIT |

## Techniques (documentation, no code copied)

The biped-override technique and its SE hook sites are documented from Skyrim Outfit System (DavidJCobb, aers, MetricExpansion), reimplemented from published documentation and public reverse-engineering facts. No source code from that project is included.
