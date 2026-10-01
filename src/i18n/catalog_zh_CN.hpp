// agentty::i18n — the Simplified Chinese catalog.
//
// The opposite extreme from German, and that is why it is third. Chinese is
// the DOUBLE-WIDTH case: 系统调用过滤 is six codepoints and twelve columns, so
// every width calculation that counts characters instead of columns is wrong
// by a factor of two here -- and wrong in the dangerous direction, because it
// UNDER-measures and lets an overflowing row through.
//
// That is why the lint's width budget (gate 4) exempts zh/ja/ko: CMake cannot
// measure display width, and a codepoint budget would fire on every correct
// translation while missing the real overflows. Their widths go through the
// render test instead, which measures with maya::string_width.
//
// Net effect on layout: Chinese is SHORTER than English in columns -- around
// 0.6x on labels -- so it relieves the pane rather than stressing it. The
// pressure it applies is on the measurement code, not on the frame.
//
// TRANSLATION NOTES:
//
//   * "Sandbox" is 沙盒, which is the established term in Chinese technical
//     writing (not a transliteration).
//
//   * Technical nouns that Chinese keeps in Latin stay in Latin: ports are
//     written as 端口 but the protocol names (https, http, git-ssh, dns) and
//     paths do not get translated. Same rule the English help lines follow
//     for `stat()` and `services/*/.env`.
//
//   * No spaces between Chinese characters, and the "·" separator keeps the
//     spaces around it -- that is the convention in zh-Hans UI text and it is
//     what keeps the help lines scannable.
//
//   * Chinese has NO plural forms (CLDR category `other` for every n), so no
//     entry here carries one/few/many. The lint rejects a `few` key on a
//     zh catalog for exactly that reason: it would be a translator
//     misunderstanding the schema, and the runtime would never read it.
//
// Complete against en.json as of this commit, which is what makes it a
// useful double-width test -- a partial catalog falls back to English and
// hides the very thing we are measuring.

#include <string_view>

namespace agentty::i18n::catalogs {

inline constexpr std::string_view kChineseSimplified = R"JSON({
  "_meta": { "lang": "zh-CN", "plurals": ["other"] },

  "lang.picker.title":   { "text": "语言" },
  "lang.picker.help":    { "text": "agentty 自身的界面文字 · 模型会用您提问时所用的语言回复" },
  "lang.auto":           { "text": "自动（跟随系统）" },
  "lang.incomplete":     { "text": "已翻译 {pct}%" },

  "appearance.theme":        { "text": "主题" },
  "appearance.scheme":       { "text": "配色方案" },
  "appearance.color":        { "text": "颜色" },
  "appearance.layout":       { "text": "布局" },
  "appearance.prose_width":  { "text": "正文宽度" },
  "appearance.compact":      { "text": "紧凑对话" },
  "appearance.motion":       { "text": "动画" },
  "appearance.content":      { "text": "内容" },
  "appearance.syntax":       { "text": "语法高亮" },

  "appearance.help.scheme":      { "text": "native 保留终端自身的颜色 · 按 Enter 浏览" },
  "appearance.help.prose_width": { "text": "正文在 N 列处换行 · 0 = 使用全部宽度" },
  "appearance.help.compact":     { "text": "去掉对话之间的空行" },
  "appearance.help.syntax":      { "text": "按语言为代码块着色" },

  "sandbox.read_paths":  { "text": "额外可读" },
  "sandbox.write_paths": { "text": "额外可写" },
  "sandbox.deny_paths":  { "text": "已屏蔽" },
  "sandbox.ports":       { "text": "允许的端口" },
  "sandbox.memory":      { "text": "内存 (MB)" },
  "sandbox.procs":       { "text": "进程数" },
  "sandbox.scan_depth":  { "text": "密钥扫描深度" },

  "sandbox.help.read_paths":  { "text": "命令可读取的额外路径，超出上面的范围设置 · 用于工作区之外的依赖" },
  "sandbox.help.write_paths": { "text": "与可读分开是有意为之：授予写入是另一个决定" },
  "sandbox.help.deny_paths":  { "text": "即使在上面的范围内也会排除 · 例如 ~/.cargo/credentials。凭据文件本来就会被屏蔽。" },
  "sandbox.help.ports":       { "text": "443 https · 80 http · 22 git-ssh · 53 dns。忘记 53 会导致一切中断。" },
  "sandbox.help.memory":      { "text": "0 = 不限制" },
  "sandbox.help.procs":       { "text": "0 = 不限制 · 可阻止 fork 炸弹" },
  "sandbox.help.scan_depth":  { "text": "0 = 仅工作区根目录 · 3 可找到 services/*/.env · 每增加一层都会让每条命令多出 stat() 调用" },

  "common.on":     { "text": "开" },
  "common.off":    { "text": "关" },
  "common.custom": { "text": "自定义" }
})JSON";

}  // namespace agentty::i18n::catalogs
