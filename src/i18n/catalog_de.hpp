// agentty::i18n — the German catalog.
//
// Why German is the SECOND language and not, say, Spanish: it is the longest.
// Measured on real agentty labels, de runs 1.35-1.61x English, and the
// pressure point is the settings panes, which are two columns inside a hard
// frame. If the layout survives German it survives every other Latin-script
// language in the set. (Chinese is the other extreme -- half the width, twice
// the columns per character -- and is the next one to land for the same
// reason.)
//
// TRANSLATION NOTES, written down because the next person to touch these will
// otherwise "fix" them:
//
//   * "Sandbox" stays "Sandbox". German tech German borrows it, and
//     "Sandkasten" means a children's play area. Same for "Tokens".
//
//   * Row labels are NOUN PHRASES, not sentences, matching the English. German
//     capitalises every noun, so "Auch lesbar" is correct where an English
//     reader might expect "Auch Lesbar" -- lesbar is an adjective.
//
//   * Help lines keep the English's lower-case opening and its "·" separators.
//     They are fragments, not sentences, and the punctuation is load-bearing
//     in the layout.
//
//   * The one deliberate shortening is appearance.help.prose_width: the
//     literal translation runs 71 columns against the English's 53, which
//     overflows the help slot at 80 columns. "Fließtext" for "assistant text"
//     is the standard typographic term and buys back 12.
//
// UNTRANSLATED ON PURPOSE: nothing here. This catalog is complete against
// en.json as of this commit, which is what makes it useful as a layout test
// -- a partial catalog would fall back to English and hide the overflow.

#include <string_view>

namespace agentty::i18n::catalogs {

inline constexpr std::string_view kGerman = R"JSON({
  "_meta": { "lang": "de", "plurals": ["one", "other"] },

  "lang.picker.title":   { "text": "Sprache" },
  "lang.picker.help":    { "text": "agenttys eigene Beschriftungen · das Modell antwortet in der Sprache, in der Sie schreiben" },
  "lang.auto":           { "text": "Automatisch (System)" },
  "lang.incomplete":     { "text": "zu {pct}% übersetzt" },

  "appearance.theme":        { "text": "Design" },
  "appearance.scheme":       { "text": "Farbschema" },
  "appearance.color":        { "text": "Farbe" },
  "appearance.layout":       { "text": "Layout" },
  "appearance.prose_width":  { "text": "Textbreite" },
  "appearance.compact":      { "text": "Kompakte Züge" },
  "appearance.motion":       { "text": "Animation" },
  "appearance.content":      { "text": "Inhalt" },
  "appearance.syntax":       { "text": "Syntaxhervorhebung" },

  "appearance.help.scheme":      { "text": "native behält die Farben Ihres Terminals · Enter zum Durchsuchen" },
  "appearance.help.prose_width": { "text": "Fließtext bei N Spalten umbrechen · 0 = volle Breite" },
  "appearance.help.compact":     { "text": "Leerzeile zwischen den Zügen weglassen" },
  "appearance.help.syntax":      { "text": "Codeblöcke nach Sprache einfärben" },

  "sandbox.read_paths":  { "text": "Auch lesbar" },
  "sandbox.write_paths": { "text": "Auch beschreibbar" },
  "sandbox.deny_paths":  { "text": "Maskiert" },
  "sandbox.ports":       { "text": "Erlaubte Ports" },
  "sandbox.memory":      { "text": "Speicher (MB)" },
  "sandbox.procs":       { "text": "Prozesse" },
  "sandbox.scan_depth":  { "text": "Suchtiefe für Geheimnisse" },

  "sandbox.help.read_paths":  { "text": "zusätzliche Pfade, die ein Befehl lesen darf, über den obigen Bereich hinaus · für Abhängigkeiten außerhalb des Arbeitsbereichs" },
  "sandbox.help.write_paths": { "text": "bewusst getrennt vom Lesen: Schreibrechte sind eine andere Entscheidung" },
  "sandbox.help.deny_paths":  { "text": "auch innerhalb des obigen Bereichs ausgenommen · z. B. ~/.cargo/credentials. Zugangsdaten werden ohnehin maskiert." },
  "sandbox.help.ports":       { "text": "443 https · 80 http · 22 git-ssh · 53 dns. 53 zu vergessen bricht alles." },
  "sandbox.help.memory":      { "text": "0 = keine Grenze" },
  "sandbox.help.procs":       { "text": "0 = keine Grenze · stoppt Fork-Bomben" },
  "sandbox.help.scan_depth":  { "text": "0 = nur das Wurzelverzeichnis · 3 findet services/*/.env · jede Ebene kostet stat()-Aufrufe bei jedem Befehl" },

  "common.on":     { "text": "ein" },
  "common.off":    { "text": "aus" },
  "common.custom": { "text": "Benutzerdefiniert" }
})JSON";

}  // namespace agentty::i18n::catalogs
