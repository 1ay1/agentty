// agentty::i18n — Japanese.
//
// The SECOND double-width language, and that is its job here. Chinese proved
// the measurement path works; Japanese proves it was not accidentally
// zh-specific. Three scripts mixed in one string (kanji 設定, hiragana かな,
// katakana サンドボックス) plus Latin tech terms, all double-width except the
// Latin, which is exactly the case a naive width implementation gets wrong.
//
// Like Chinese: NO plural forms (CLDR `other` for every n), so no entry here
// carries one/few/many. The lint rejects a `few` key on a ja catalog because
// the runtime would never read it -- that is a translator misreading the
// schema, and silence would be worse than a build failure.
//
// TRANSLATION NOTES:
//
//   * サンドボックス (katakana) for "sandbox" -- the established loan, not a
//     native coinage.
//
//   * No spaces between Japanese characters. The "·" separator keeps its
//     surrounding spaces because it sits between phrases, which is the
//     convention in Japanese UI text that mixes Latin punctuation.
//
//   * Help lines end without a period. Japanese UI microcopy conventionally
//     omits 。 on fragments, and these are fragments.
//
//   * ～ (wave dash) is deliberately avoided: it renders inconsistently
//     across terminal fonts and has a notorious Unicode normalisation trap
//     (U+301C vs U+FF5E). Nothing here needs it.

#include <string_view>

namespace agentty::i18n::catalogs {

inline constexpr std::string_view kJapanese = R"JSON({
  "_meta": { "lang": "ja", "plurals": ["other"] },

  "lang.picker.title":   { "text": "言語" },
  "lang.picker.help":    { "text": "agentty 自身のラベル · モデルはあなたが書いた言語で返答します" },
  "lang.auto":           { "text": "自動（システムに従う）" },
  "lang.incomplete":     { "text": "{pct}% 翻訳済み" },

  "appearance.theme":        { "text": "テーマ" },
  "appearance.scheme":       { "text": "配色" },
  "appearance.color":        { "text": "色" },
  "appearance.layout":       { "text": "レイアウト" },
  "appearance.prose_width":  { "text": "本文の幅" },
  "appearance.compact":      { "text": "コンパクト表示" },
  "appearance.motion":       { "text": "アニメーション" },
  "appearance.content":      { "text": "内容" },
  "appearance.syntax":       { "text": "構文強調" },

  "appearance.help.scheme":      { "text": "native は端末自身の色を保ちます · Enter で一覧" },
  "appearance.help.prose_width": { "text": "本文を N 桁で折り返す · 0 = 全幅" },
  "appearance.help.compact":     { "text": "ターンの間の空行を省く" },
  "appearance.help.syntax":      { "text": "コードブロックを言語ごとに色分けする" },

  "sandbox.read_paths":  { "text": "追加の読み取り" },
  "sandbox.write_paths": { "text": "追加の書き込み" },
  "sandbox.deny_paths":  { "text": "マスク済み" },
  "sandbox.ports":       { "text": "許可ポート" },
  "sandbox.memory":      { "text": "メモリ (MB)" },
  "sandbox.procs":       { "text": "プロセス数" },
  "sandbox.scan_depth":  { "text": "機密の探索深度" },

  "sandbox.help.read_paths":  { "text": "上の範囲設定に加えてコマンドが読み取れるパス · ワークスペース外の依存関係のため" },
  "sandbox.help.write_paths": { "text": "読み取りとは意図的に分けています。書き込みの許可は別の判断です" },
  "sandbox.help.deny_paths":  { "text": "上の範囲内でも除外されます · 例 ~/.cargo/credentials。認証情報はいずれにせよマスクされます" },
  "sandbox.help.ports":       { "text": "443 https · 80 http · 22 git-ssh · 53 dns。53 を忘れるとすべて壊れます" },
  "sandbox.help.memory":      { "text": "0 = 制限なし" },
  "sandbox.help.procs":       { "text": "0 = 制限なし · fork 爆弾を止めます" },
  "sandbox.help.scan_depth":  { "text": "0 = ワークスペース直下のみ · 3 で services/*/.env を検出 · 階層ごとに各コマンドで stat() が増えます" },

  "common.on":     { "text": "オン" },
  "common.off":    { "text": "オフ" },
  "common.custom": { "text": "カスタム" }
})JSON";

}  // namespace agentty::i18n::catalogs
