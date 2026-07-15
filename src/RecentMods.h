#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace OS::RecentMods {

    // "Recently added" detection (OS-26): a style is flagged NEW when its
    // defining plugin was NOT present at the previous launch. The set of
    // plugins seen last launch persists in a GLOBAL flat file
    // (Data/SKSE/Plugins/FittingRoom/known_plugins.txt), same doctrine as
    // favorites.txt / crashed_styles.txt. On the very FIRST run (no file) the
    // set is seeded silently and nothing is flagged - otherwise the whole
    // catalog would light up as new. The flag naturally clears the launch after
    // a mod is added, once its plugin joins the baseline.

    // kDataLoaded, before StyleCatalog::Build(): load last launch's plugin set.
    void LoadAtStartup();

    // Was this plugin absent last launch? Always false on the first run (no
    // baseline to diff against). Queried per style during Build().
    [[nodiscard]] bool IsNewPlugin(std::string_view a_plugin);

    // Persist THIS launch's plugin set (the sources that have styles) as the
    // new baseline, so today's "new" plugins are known next launch. Call once
    // after Build() has the full source list.
    void CommitSeen(const std::vector<std::string>& a_plugins);

    // How many of this launch's plugins were newly added (for the log line).
    [[nodiscard]] std::size_t NewCount();

}  // namespace OS::RecentMods
