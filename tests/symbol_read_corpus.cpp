// symbol_read_corpus — replay real `read symbol=` lookups that failed in
// saved sessions against the CURRENT tool. Each line of the corpus file is
// `<abs path>\t<symbol>`; the symbol is known to appear in the file. Reports
// how many resolve, and prints the misses. Also pass --verbose to show the
// first line of each resolved span (sanity: it should be the definition).
//   agentty_standalone_tests symbol_read_corpus <corpus.tsv> [--verbose]

#include <cstdio>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "agentty/tool/mcp_tools_bridge.hpp"
#include "agentty/tool/registry.hpp"
#include "agentty/tool/util/fs_helpers.hpp"

using nlohmann::json;
using namespace agentty;

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: symbol_read_corpus <corpus.tsv> [--verbose]\n"); return 2; }
    const bool verbose = argc > 2 && std::string{argv[2]} == "--verbose";
    tools::util::set_workspace_root("/");
    tools::wire_mcp_runtime("off");
    const auto* read = tools::find("read");
    if (!read) { std::printf("read tool not registered\n"); return 1; }
    std::ifstream in(argv[1]);
    std::string line;
    int total = 0, ok = 0;
    while (std::getline(in, line)) {
        auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        const std::string path = line.substr(0, tab), sym = line.substr(tab + 1);
        ++total;
        auto r = read->execute(json{{"path", path}, {"symbol", sym}});
        if (r && r->text.find("no definition of") == std::string::npos) {
            ++ok;
            if (verbose) {
                auto body = r->text;
                auto nl = body.find('\n');
                auto nl2 = nl == std::string::npos ? nl : body.find('\n', nl + 1);
                auto nl3 = nl2 == std::string::npos ? nl2 : body.find('\n', nl2 + 1);
                std::printf("OK   %-28s %s\n", sym.c_str(),
                            body.substr(nl2 == std::string::npos ? 0 : nl2 + 1,
                                        nl3 == std::string::npos ? 100 : std::min<std::size_t>(100, nl3 - nl2 - 1)).c_str());
            }
        } else {
            std::printf("MISS %-28s %s\n", sym.c_str(), path.c_str());
        }
    }
    std::printf("resolved %d / %d\n", ok, total);
    return 0;
}
