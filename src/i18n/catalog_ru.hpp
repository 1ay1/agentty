// agentty::i18n — Russian.
//
// The THREE-FORM PLURAL case, and the reason it is in this batch. Every
// language shipped so far has one form (zh, ja) or two (en, de, es, fr);
// Russian is the first with `one`/`few`/`many`, which is the case
// `"%d file(s)"` cannot express and the reason this project does not use
// gettext's runtime-evaluated rules.
//
//     1 файл      one     ends in 1, but NOT 11
//     2 файла     few     ends in 2-4, but NOT 12-14
//     5 файлов    many    everything else
//     21 файл     one     again -- the LAST DIGIT decides
//
// The catalog carries no plural strings yet (none of the 34 ids are counted
// nouns), so this is currently untested against real Russian text. The
// SELECTOR is tested exhaustively -- every integer 0..1000 against an
// independently written rule, in i18n_test -- which is the half that would
// be wrong. When the first counted string lands, `_meta.plurals` here is
// what tells the translator they owe three forms.
//
// Also the first CYRILLIC catalog. Cyrillic is single-width (verified:
// string_width("при") == 3), so it behaves like Latin for layout -- but it
// runs ~1.3x English, close to German, so the width budget matters.
//
// TRANSLATION NOTES:
//
//   * «песочница» is the established Russian term for a sandbox in security
//     writing, not a loan. Used in the help text; the pane title stays in
//     English because it is a proper noun there.
//
//   * ты, not вы. Dev-tool register, same call as Spanish and French.
//
//   * Latin tech terms (https, git-ssh, dns, stat(), fork) stay Latin --
//     that is how they are written in Russian technical prose.

#include <string_view>

namespace agentty::i18n::catalogs {

inline constexpr std::string_view kRussian = R"JSON({
  "_meta": { "lang": "ru", "plurals": ["one", "few", "many"] },

  "lang.picker.title":   { "text": "Язык" },
  "lang.picker.help":    { "text": "собственные подписи agentty · модель отвечает на том языке, на котором ты пишешь" },
  "lang.auto":           { "text": "Автоматически (система)" },
  "lang.incomplete":     { "text": "переведено на {pct}%" },

  "appearance.theme":        { "text": "Оформление" },
  "appearance.scheme":       { "text": "Схема" },
  "appearance.color":        { "text": "Цвет" },
  "appearance.layout":       { "text": "Макет" },
  "appearance.prose_width":  { "text": "Ширина текста" },
  "appearance.compact":      { "text": "Компактно" },
  "appearance.motion":       { "text": "Анимация" },
  "appearance.content":      { "text": "Содержимое" },
  "appearance.syntax":       { "text": "Подсветка синтаксиса" },

  "appearance.help.scheme":      { "text": "native сохраняет цвета твоего терминала · Enter для просмотра" },
  "appearance.help.prose_width": { "text": "переносить текст на N колонках · 0 = вся ширина" },
  "appearance.help.compact":     { "text": "убрать пустую строку между ходами" },
  "appearance.help.syntax":      { "text": "раскрашивать блоки кода по языку" },

  "sandbox.read_paths":  { "text": "Также читаемо" },
  "sandbox.write_paths": { "text": "Также записываемо" },
  "sandbox.deny_paths":  { "text": "Скрыто" },
  "sandbox.ports":       { "text": "Открытые порты" },
  "sandbox.memory":      { "text": "Память (МБ)" },
  "sandbox.procs":       { "text": "Процессы" },
  "sandbox.scan_depth":  { "text": "Глубина поиска секретов" },

  "sandbox.help.read_paths":  { "text": "дополнительные пути, которые команда может читать, сверх области выше · для зависимостей вне рабочей папки" },
  "sandbox.help.write_paths": { "text": "намеренно отделено от чтения: доступ на запись — отдельное решение" },
  "sandbox.help.deny_paths":  { "text": "исключено даже внутри области выше · напр. ~/.cargo/credentials. учётные данные скрываются в любом случае." },
  "sandbox.help.ports":       { "text": "443 https · 80 http · 22 git-ssh · 53 dns. забыть 53 — сломать всё." },
  "sandbox.help.memory":      { "text": "0 = без ограничения" },
  "sandbox.help.procs":       { "text": "0 = без ограничения · останавливает fork-бомбы" },
  "sandbox.help.scan_depth":  { "text": "0 = только корень рабочей папки · 3 находит services/*/.env · каждый уровень стоит вызовов stat() на каждой команде" },

  "common.on":     { "text": "вкл" },
  "common.off":    { "text": "выкл" },
  "common.custom": { "text": "Своё" }
})JSON";

}  // namespace agentty::i18n::catalogs
