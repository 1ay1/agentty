// agentty::i18n — Portuguese (Brazil).
//
// pt-BR specifically, not pt. The two differ in vocabulary where it matters
// for a dev tool -- "tela" vs "ecrã", "arquivo" vs "ficheiro" -- and this
// catalog uses the Brazilian forms throughout. parse_tag maps pt-PT here as
// well, which is a deliberate trade: a European Portuguese speaker reading
// Brazilian forms is far better served than one reading English. (Contrast
// zh-TW, which does NOT fall back to zh-CN -- see parse_tag for why the two
// cases differ.)
//
// TRANSLATION NOTES:
//
//   * "arquivo" not "ficheiro", "tela" not "ecrã", "usuário" not "utilizador".
//     The pt-PT reader will notice; they will also understand every word.
//
//   * você, with verbs conjugated third-person. That is the neutral
//     Brazilian register for software, and it sidesteps the tu/você regional
//     split entirely.
//
//   * "Sandbox" stays. "Caixa de areia" is a literal translation that reads
//     as a children's sandpit in exactly the way the English does not.

#include <string_view>

namespace agentty::i18n::catalogs {

inline constexpr std::string_view kPortugueseBR = R"JSON({
  "_meta": { "lang": "pt-BR", "plurals": ["one", "other"] },

  "lang.picker.title":   { "text": "Idioma" },
  "lang.picker.help":    { "text": "os rótulos do próprio agentty · o modelo responde no idioma em que você escrever" },
  "lang.auto":           { "text": "Automático (do sistema)" },
  "lang.incomplete":     { "text": "{pct}% traduzido" },

  "appearance.theme":        { "text": "Tema" },
  "appearance.scheme":       { "text": "Esquema" },
  "appearance.color":        { "text": "Cor" },
  "appearance.layout":       { "text": "Layout" },
  "appearance.prose_width":  { "text": "Largura do texto" },
  "appearance.compact":      { "text": "Turnos compactos" },
  "appearance.motion":       { "text": "Animação" },
  "appearance.content":      { "text": "Conteúdo" },
  "appearance.syntax":       { "text": "Realce de sintaxe" },

  "appearance.help.scheme":      { "text": "native mantém as cores do seu terminal · Enter para explorar" },
  "appearance.help.prose_width": { "text": "quebrar o texto em N colunas · 0 = largura total" },
  "appearance.help.compact":     { "text": "remover a linha em branco entre os turnos" },
  "appearance.help.syntax":      { "text": "colorir os blocos de código por linguagem" },

  "sandbox.read_paths":  { "text": "Também legível" },
  "sandbox.write_paths": { "text": "Também gravável" },
  "sandbox.deny_paths":  { "text": "Ocultos" },
  "sandbox.ports":       { "text": "Portas permitidas" },
  "sandbox.memory":      { "text": "Memória (MB)" },
  "sandbox.procs":       { "text": "Processos" },
  "sandbox.scan_depth":  { "text": "Profundidade da busca" },

  "sandbox.help.read_paths":  { "text": "caminhos adicionais que um comando pode ler, além do escopo acima · para dependências fora do espaço de trabalho" },
  "sandbox.help.write_paths": { "text": "separado da leitura de propósito: conceder escrita é outra decisão" },
  "sandbox.help.deny_paths":  { "text": "excluídos mesmo dentro do escopo acima · ex. ~/.cargo/credentials. as credenciais são ocultadas de qualquer forma." },
  "sandbox.help.ports":       { "text": "443 https · 80 http · 22 git-ssh · 53 dns. esquecer a 53 quebra tudo." },
  "sandbox.help.memory":      { "text": "0 = sem limite" },
  "sandbox.help.procs":       { "text": "0 = sem limite · barra fork bombs" },
  "sandbox.help.scan_depth":  { "text": "0 = só a raiz do espaço de trabalho · 3 encontra services/*/.env · cada nível custa chamadas stat() em cada comando" },

  "common.on":     { "text": "sim" },
  "common.off":    { "text": "não" },
  "common.custom": { "text": "Personalizado" }
})JSON";

}  // namespace agentty::i18n::catalogs
