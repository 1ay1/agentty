// agentty::rag — Porter stemmer for BM25 vocabulary normalization.
// Implements the classic Porter (1980) algorithm for English, pure C++/STL.
// OPT-IN: disabled by default. Enable via BM25_USE_STEMMER=1 or call
// stem() explicitly. Reduces "deployment" → "deploy", "configured" → "configur".

#include "agentty/rag/stemmer.hpp"

#include <algorithm>
#include <string>
#include <string_view>

namespace agentty::rag {

namespace {

// Vowel check (y is contextually a vowel but handled specially).
bool is_vowel(char c) {
    return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u';
}

bool is_consonant(const std::string& s, std::size_t i) {
    if (i >= s.size()) return false;
    char c = s[i];
    if (is_vowel(c)) return false;
    if (c == 'y') {
        if (i == 0) return true;
        return !is_consonant(s, i - 1);
    }
    return true;
}

// Measure m: count of VC sequences in the stem.
int measure(const std::string& s) {
    int m = 0;
    std::size_t i = 0;
    // Skip leading consonants.
    while (i < s.size() && is_consonant(s, i)) ++i;
    while (i < s.size()) {
        // Skip vowels.
        while (i < s.size() && !is_consonant(s, i)) ++i;
        if (i >= s.size()) break;
        // Skip consonants.
        while (i < s.size() && is_consonant(s, i)) ++i;
        ++m;
    }
    return m;
}

// Contains a vowel?
bool has_vowel(const std::string& s) {
    for (std::size_t i = 0; i < s.size(); ++i)
        if (!is_consonant(s, i)) return true;
    return false;
}

// Ends with double consonant?
bool ends_double_consonant(const std::string& s) {
    if (s.size() < 2) return false;
    return s[s.size() - 1] == s[s.size() - 2] && is_consonant(s, s.size() - 1);
}

// Ends with CVC where final C is not w/x/y?
bool ends_cvc(const std::string& s) {
    if (s.size() < 3) return false;
    std::size_t n = s.size();
    if (!is_consonant(s, n - 1) || is_consonant(s, n - 2) || !is_consonant(s, n - 3))
        return false;
    char c = s[n - 1];
    return c != 'w' && c != 'x' && c != 'y';
}

bool ends_with(const std::string& s, std::string_view suffix) {
    if (s.size() < suffix.size()) return false;
    return s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

void step1a(std::string& s) {
    if (ends_with(s, "sses")) { s.resize(s.size() - 2); return; }
    if (ends_with(s, "ies"))  { s.resize(s.size() - 2); return; }
    if (ends_with(s, "ss"))   { return; }
    if (ends_with(s, "s"))    { s.pop_back(); return; }
}

void step1b(std::string& s) {
    bool flag = false;
    if (ends_with(s, "eed")) {
        std::string base = s.substr(0, s.size() - 3);
        if (measure(base) > 0) s.resize(s.size() - 2);
    } else if (ends_with(s, "ed")) {
        std::string base = s.substr(0, s.size() - 2);
        if (has_vowel(base)) { s.resize(s.size() - 2); flag = true; }
    } else if (ends_with(s, "ing")) {
        std::string base = s.substr(0, s.size() - 3);
        if (has_vowel(base)) { s.resize(s.size() - 3); flag = true; }
    }
    if (flag) {
        if (ends_with(s, "at") || ends_with(s, "bl") || ends_with(s, "iz")) {
            s.push_back('e');
        } else if (ends_double_consonant(s) && !ends_with(s, "l") &&
                   !ends_with(s, "s") && !ends_with(s, "z")) {
            s.pop_back();
        } else if (measure(s) == 1 && ends_cvc(s)) {
            s.push_back('e');
        }
    }
}

void step1c(std::string& s) {
    if (!s.empty() && s.back() == 'y') {
        std::string base = s.substr(0, s.size() - 1);
        if (has_vowel(base)) s.back() = 'i';
    }
}

void step2(std::string& s) {
    static const std::pair<std::string_view, std::string_view> rules[] = {
        {"ational", "ate"}, {"tional", "tion"}, {"enci", "ence"}, {"anci", "ance"},
        {"izer", "ize"}, {"abli", "able"}, {"alli", "al"}, {"entli", "ent"},
        {"eli", "e"}, {"ousli", "ous"}, {"ization", "ize"}, {"ation", "ate"},
        {"ator", "ate"}, {"alism", "al"}, {"iveness", "ive"}, {"fulness", "ful"},
        {"ousness", "ous"}, {"aliti", "al"}, {"iviti", "ive"}, {"biliti", "ble"},
    };
    for (auto& [from, to] : rules) {
        if (ends_with(s, from)) {
            std::string base = s.substr(0, s.size() - from.size());
            if (measure(base) > 0) {
                s.resize(s.size() - from.size());
                s.append(to);
            }
            return;
        }
    }
}

void step3(std::string& s) {
    static const std::pair<std::string_view, std::string_view> rules[] = {
        {"icate", "ic"}, {"ative", ""}, {"alize", "al"}, {"iciti", "ic"},
        {"ical", "ic"}, {"ful", ""}, {"ness", ""},
    };
    for (auto& [from, to] : rules) {
        if (ends_with(s, from)) {
            std::string base = s.substr(0, s.size() - from.size());
            if (measure(base) > 0) {
                s.resize(s.size() - from.size());
                s.append(to);
            }
            return;
        }
    }
}

void step4(std::string& s) {
    static const std::string_view suffixes[] = {
        "al", "ance", "ence", "er", "ic", "able", "ible", "ant", "ement",
        "ment", "ent", "ion", "ou", "ism", "ate", "iti", "ous", "ive", "ize",
    };
    for (auto suf : suffixes) {
        if (ends_with(s, suf)) {
            std::string base = s.substr(0, s.size() - suf.size());
            if (suf == "ion") {
                if (!base.empty() && (base.back() == 's' || base.back() == 't') &&
                    measure(base) > 1) {
                    s.resize(s.size() - suf.size());
                }
            } else if (measure(base) > 1) {
                s.resize(s.size() - suf.size());
            }
            return;
        }
    }
}

void step5a(std::string& s) {
    if (!s.empty() && s.back() == 'e') {
        std::string base = s.substr(0, s.size() - 1);
        if (measure(base) > 1) s.pop_back();
        else if (measure(base) == 1 && !ends_cvc(base)) s.pop_back();
    }
}

void step5b(std::string& s) {
    if (measure(s) > 1 && ends_double_consonant(s) && s.back() == 'l') {
        s.pop_back();
    }
}

} // namespace

std::string stem(std::string_view word) {
    if (word.size() < 3) return std::string{word};
    
    std::string s{word};
    // Lowercase.
    for (auto& c : s) c = static_cast<char>(std::tolower((unsigned char)c));

    step1a(s);
    step1b(s);
    step1c(s);
    step2(s);
    step3(s);
    step4(s);
    step5a(s);
    step5b(s);

    return s;
}

std::vector<std::string> stem_tokens(const std::vector<std::string>& tokens) {
    std::vector<std::string> out;
    out.reserve(tokens.size());
    for (const auto& t : tokens) out.push_back(stem(t));
    return out;
}

} // namespace agentty::rag
