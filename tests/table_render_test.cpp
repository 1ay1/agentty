// table_render_test — verify maya's streaming markdown classifies a
// well-formed GFM table as Table, and a malformed one (prose before the
// header pipe, the shape the model produced) as Paragraph. This is the
// exact failure the user saw: the renderer is spec-correct, so the fix
// lives in the system prompt, not here. This test pins the renderer
// behaviour so we know which shape renders as a grid.
#include <string>

#include "agtest.hpp"

#include <maya/widget/markdown.hpp>

using K = maya::StreamingMarkdown::BlockKind;

static const char* kind_name(K k) {
    switch (k) {
        case K::Paragraph: return "Paragraph";
        case K::Heading:   return "Heading";
        case K::CodeBlock: return "CodeBlock";
        case K::Blockquote:return "Blockquote";
        case K::List:      return "List";
        case K::HRule:     return "HRule";
        case K::Table:     return "Table";
        default:           return "Other";
    }
}

static bool has_table(const std::string& src) {
    maya::StreamingMarkdown md;
    md.set_content(src);
    md.finish();
    bool found = false;
    std::printf("  source:\n");
    // echo source indented
    {
        std::string line;
        for (char c : src) {
            if (c == '\n') { std::printf("    | %s\n", line.c_str()); line.clear(); }
            else line += c;
        }
        if (!line.empty()) std::printf("    | %s\n", line.c_str());
    }
    std::printf("  blocks: ");
    for (std::size_t i = 0; i < md.block_count(); ++i) {
        K k = md.block_meta(i).kind;
        std::printf("%s ", kind_name(k));
        if (k == K::Table) found = true;
    }
    std::printf("\n");
    return found;
}

TEST_CASE("table render") {
    
    // ── 1. WELL-FORMED table: header line starts at '|', blank line
    //    before it. Must classify as Table.
    std::printf("[well-formed table]\n");
    const std::string good =
        "Source layout (src/):\n"
        "\n"
        "| Dir | Likely role |\n"
        "|-----|-------------|\n"
        "| airgap/ | SSH airgap / SOCKS5 relay |\n"
        "| diff/ | Diff rendering for edits |\n";
    CHECK_MESSAGE(has_table(good), "expected a Table block, got none");

    // ── 2. MALFORMED (the model's shape): lead-in prose on the SAME line
    //    as the header. GFM rejects it → Paragraph (wall of pipes). This
    //    is what the user saw; the renderer is correct.
    std::printf("\n[malformed: prose before header pipe]\n");
    const std::string bad =
        "Source layout (src/): | Dir | Likely role | airgap/ | SSH airgap |\n"
        "|---|---|\n"
        "| diff/ | Diff rendering |\n";
    CHECK_MESSAGE(!has_table(bad),
                  "parser accepted a non-conformant table (bug is elsewhere)");
}
