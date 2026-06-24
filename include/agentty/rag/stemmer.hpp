#pragma once
// agentty::rag — Porter stemmer for BM25 vocabulary normalization.
//
// The Porter stemmer (Porter 1980) is the classic English stemmer: it reduces
// inflected/derived words to a common base form, improving recall when the
// query and the document use different word forms ("deploy" vs "deployment").
//
// This is OPT-IN for BM25: stemming can hurt precision on proper nouns and
// technical terms where the exact surface form matters. Enable explicitly
// when the knowledge corpus is prose-heavy (manuals, wikis, articles).

#include <string>
#include <string_view>
#include <vector>

namespace agentty::rag {

// Stem a single word using the Porter algorithm. Returns the stemmed form.
// Short words (< 3 chars) pass through unchanged.
[[nodiscard]] std::string stem(std::string_view word);

// Stem a list of tokens. Returns a parallel list of stemmed tokens.
[[nodiscard]] std::vector<std::string> stem_tokens(const std::vector<std::string>& tokens);

} // namespace agentty::rag
