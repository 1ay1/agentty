// agentty::i18n — Spanish.
//
// The first of the "ordinary" Latin catalogs: two plural forms, single-width
// script, ~1.2x English. Nothing structurally new, which is the point --
// German and Chinese established the extremes, and these five are the ones
// that confirm the middle is uneventful.
//
// TRANSLATION NOTES:
//
//   * "Sandbox" stays. Spanish technical writing borrows it; "caja de arena"
//     is a literal translation nobody uses for this.
//
//   * tú, not usted. agentty is a developer tool and the register in
//     Spanish-language dev documentation is informal -- "usted" would read
//     as a bank.
//
//   * Toggle values are sí/no rather than activado/desactivado: the row is
//     four columns wide and the long form does not fit. The budget in
//     startup.cpp says so (common.on, width_hint 2).

#include <string_view>

namespace agentty::i18n::catalogs {

inline constexpr std::string_view kSpanish = R"JSON({
  "_meta": { "lang": "es", "plurals": ["one", "other"] },

  "lang.picker.title":   { "text": "Idioma" },
  "lang.picker.help":    { "text": "las etiquetas propias de agentty · el modelo responde en el idioma en que le escribas" },
  "lang.auto":           { "text": "Automático (del sistema)" },
  "lang.incomplete":     { "text": "{pct}% traducido" },

  "appearance.theme":        { "text": "Tema" },
  "appearance.scheme":       { "text": "Esquema" },
  "appearance.color":        { "text": "Color" },
  "appearance.layout":       { "text": "Diseño" },
  "appearance.prose_width":  { "text": "Ancho del texto" },
  "appearance.compact":      { "text": "Turnos compactos" },
  "appearance.motion":       { "text": "Movimiento" },
  "appearance.content":      { "text": "Contenido" },
  "appearance.syntax":       { "text": "Resaltado de sintaxis" },

  "appearance.help.scheme":      { "text": "native conserva los colores de tu terminal · Enter para explorar" },
  "appearance.help.prose_width": { "text": "ajustar el texto a N columnas · 0 = todo el ancho" },
  "appearance.help.compact":     { "text": "quitar la línea en blanco entre turnos" },
  "appearance.help.syntax":      { "text": "colorear los bloques de código por lenguaje" },

  "sandbox.read_paths":  { "text": "También legible" },
  "sandbox.write_paths": { "text": "También escribible" },
  "sandbox.deny_paths":  { "text": "Ocultos" },
  "sandbox.ports":       { "text": "Puertos permitidos" },
  "sandbox.memory":      { "text": "Memoria (MB)" },
  "sandbox.procs":       { "text": "Procesos" },
  "sandbox.scan_depth":  { "text": "Profundidad de búsqueda" },

  "sandbox.help.read_paths":  { "text": "rutas adicionales que un comando puede leer, más allá del alcance de arriba · para dependencias fuera del espacio de trabajo" },
  "sandbox.help.write_paths": { "text": "separado de la lectura a propósito: conceder escritura es otra decisión" },
  "sandbox.help.deny_paths":  { "text": "excluidos incluso dentro del alcance de arriba · p. ej. ~/.cargo/credentials. las credenciales se ocultan igualmente." },
  "sandbox.help.ports":       { "text": "443 https · 80 http · 22 git-ssh · 53 dns. olvidar el 53 lo rompe todo." },
  "sandbox.help.memory":      { "text": "0 = sin límite" },
  "sandbox.help.procs":       { "text": "0 = sin límite · detiene las fork bombs" },
  "sandbox.help.scan_depth":  { "text": "0 = solo la raíz del espacio de trabajo · 3 encuentra services/*/.env · cada nivel cuesta llamadas a stat() en cada comando" },

  "common.on":     { "text": "sí" },
  "common.off":    { "text": "no" },
  "common.custom": { "text": "Personalizado" }
})JSON";

}  // namespace agentty::i18n::catalogs
