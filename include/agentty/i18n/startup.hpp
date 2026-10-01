#pragma once
// agentty::i18n — startup: choosing a language and loading the catalogs.
//
// Split from i18n.hpp so the core stays free of getenv and IO, and so a test
// can drive resolve_language() with explicit inputs rather than by mutating
// the process environment.

#include <string_view>

#include "agentty/i18n/i18n.hpp"

namespace agentty::i18n {

// Which language to use, given the flag and the saved setting.
//
// Precedence, highest first:
//
//   1. --lang          what the user typed THIS run
//   2. settings.json   ui.lang, when non-empty (empty = "auto")
//   3. environment     LC_ALL, LC_MESSAGES, LANGUAGE, LANG
//   4. en
//
// Pure: takes its inputs as arguments rather than reading settings, so the
// precedence can be tested without a filesystem or an environment.
[[nodiscard]] Lang resolve_language(std::string_view cli_lang,
                                    std::string_view saved);

// Load the embedded catalogs and activate the resolved language.
//
// Returns false only when the ENGLISH catalog fails to parse, which is a
// build defect rather than a runtime condition -- a requested language with
// no catalog is not an error, it falls back to English silently (the picker
// shows completeness, so the gap is visible where it is actionable).
bool init(std::string_view cli_lang, std::string_view saved);

}  // namespace agentty::i18n
