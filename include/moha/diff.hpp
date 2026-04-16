#pragma once

#include <string>
#include <vector>

#include "moha/model.hpp"

namespace moha::diff {

// Compute unified diff and structured hunks from before/after text.
FileChange compute(const std::string& path,
                   const std::string& before,
                   const std::string& after);

// Render a unified diff string (for display / for passing to DiffView widget).
std::string render_unified(const FileChange& c);

// Apply accepted hunks on top of `original_contents`, skipping rejected ones.
std::string apply_accepted(const FileChange& c);

} // namespace moha::diff
