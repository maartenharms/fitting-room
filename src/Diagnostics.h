#pragma once

namespace OS::Diagnostics {

    // Warn (log + message box) when a mod that overrides worn-armor rendering
    // the same way we do is installed. Same-territory conflicts are inherent to
    // the whole transmog family; the spec requires surfacing them at startup.
    // Call at kDataLoaded, after StyleCatalog::Build().
    void WarnOnConflicts();

}  // namespace OS::Diagnostics
