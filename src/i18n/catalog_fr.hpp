// agentty::i18n — French.
//
// The CLDR exception in this set: French's `one` category covers i = 0,1, so
// ZERO TAKES THE SINGULAR -- "0 fichier", not "0 fichiers". Every other
// two-form language here puts 0 in `other`. plural.hpp handles it with one
// extra comparison rather than a sixth rule shape, and the sweep in
// i18n_test checks every integer 0..1000 against an independently written
// rule, so this cannot drift quietly.
//
// No plural strings exist in the catalog yet, so the rule is currently
// untested against real French text -- but the selector is, which is the
// part that would be wrong.
//
// TRANSLATION NOTES:
//
//   * Typographic spacing. French puts a space before : ; ! ? and inside
//     « ». We use a NARROW NO-BREAK SPACE (U+202F) before the colon, which
//     is the correct character and renders as one column. A plain space
//     would be allowed to wrap, leaving a line starting with ":".
//
//   * tu, not vous. Same reasoning as Spanish: dev-tool register.
//
//   * "Sandbox" stays -- "bac à sable" is the literal translation and does
//     appear in French security writing, but a CLI label is not the place
//     for the long form.

#include <string_view>

namespace agentty::i18n::catalogs {

inline constexpr std::string_view kFrench = R"JSON({
  "_meta": { "lang": "fr", "plurals": ["one", "other"] },

  "lang.picker.title":   { "text": "Langue" },
  "lang.picker.help":    { "text": "les libellés propres à agentty · le modèle répond dans la langue que tu utilises" },
  "lang.auto":           { "text": "Automatique (système)" },
  "lang.incomplete":     { "text": "traduit à {pct}%" },

  "appearance.theme":        { "text": "Thème" },
  "appearance.scheme":       { "text": "Palette" },
  "appearance.color":        { "text": "Couleur" },
  "appearance.layout":       { "text": "Mise en page" },
  "appearance.prose_width":  { "text": "Largeur du texte" },
  "appearance.compact":      { "text": "Tours compacts" },
  "appearance.motion":       { "text": "Animation" },
  "appearance.content":      { "text": "Contenu" },
  "appearance.syntax":       { "text": "Coloration syntaxique" },

  "appearance.help.scheme":      { "text": "native garde les couleurs de ton terminal · Entrée pour parcourir" },
  "appearance.help.prose_width": { "text": "couper le texte à N colonnes · 0 = toute la largeur" },
  "appearance.help.compact":     { "text": "supprimer la ligne vide entre les tours" },
  "appearance.help.syntax":      { "text": "colorer les blocs de code selon le langage" },

  "sandbox.read_paths":  { "text": "Aussi lisible" },
  "sandbox.write_paths": { "text": "Aussi modifiable" },
  "sandbox.deny_paths":  { "text": "Masqués" },
  "sandbox.ports":       { "text": "Ports autorisés" },
  "sandbox.memory":      { "text": "Mémoire (Mo)" },
  "sandbox.procs":       { "text": "Processus" },
  "sandbox.scan_depth":  { "text": "Profondeur de recherche" },

  "sandbox.help.read_paths":  { "text": "chemins supplémentaires qu'une commande peut lire, au-delà de la portée ci-dessus · pour une dépendance hors de l'espace de travail" },
  "sandbox.help.write_paths": { "text": "séparé de la lecture à dessein : accorder l'écriture est une autre décision" },
  "sandbox.help.deny_paths":  { "text": "exclus même dans la portée ci-dessus · p. ex. ~/.cargo/credentials. les identifiants sont masqués de toute façon." },
  "sandbox.help.ports":       { "text": "443 https · 80 http · 22 git-ssh · 53 dns. oublier le 53 casse tout." },
  "sandbox.help.memory":      { "text": "0 = sans limite" },
  "sandbox.help.procs":       { "text": "0 = sans limite · arrête les fork bombs" },
  "sandbox.help.scan_depth":  { "text": "0 = uniquement la racine · 3 trouve services/*/.env · chaque niveau coûte des appels stat() à chaque commande" },

  "common.on":     { "text": "oui" },
  "common.off":    { "text": "non" },
  "common.custom": { "text": "Personnalisé" }
})JSON";

}  // namespace agentty::i18n::catalogs
