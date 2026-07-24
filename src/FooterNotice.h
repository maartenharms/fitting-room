#pragma once

#include <string>
#include <string_view>

namespace OS::FooterNotice {

    // Never put the unbounded filesystem path in the shared footer row. A
    // short localized success message gives immediate feedback without
    // colliding with Saved/Close; failures remain visible because they
    // require action.
    [[nodiscard]] inline std::string ExportResult(
        std::string_view a_path, std::string_view a_successText,
        std::string_view a_failureText) {
        return std::string{ a_path.empty() ? a_failureText : a_successText };
    }

}  // namespace OS::FooterNotice
