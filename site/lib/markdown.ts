// Tiny, dependency-free Markdown → HTML for the blog. Deliberately NOT remark/
// mdx: the site ships zero runtime deps and renders docs as hand-written JSX, so
// a blog post is just a Markdown string turned into a safe HTML string at build
// time. Supports the subset a changelog/announcement post actually needs:
// headings, paragraphs, fenced + inline code, bold/italic, links, images,
// ordered/unordered lists, blockquotes, hr, and tables. Output is sanitized by
// construction (we escape all text; we only emit a fixed tag set).

const esc = (s: string) =>
  s
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;");

// Inline: code spans first (so their contents aren't further formatted), then
// images, links, bold, italic. Operates on already-escaped text.
function inline(text: string): string {
  // pull out `code` spans, replace with placeholders, restore at the end
  const codes: string[] = [];
  let out = text.replace(/`([^`]+)`/g, (_m, c) => {
    codes.push(`<code>${c}</code>`);
    return `\u0000${codes.length - 1}\u0000`;
  });

  out = out
    // keyboard chips [[Ctrl+K]] -> <kbd>Ctrl+K</kbd>
    .replace(/\[\[([^\]]+)\]\]/g, (_m, k) => `<kbd>${k}</kbd>`)
    // images ![alt](src)
    .replace(/!\[([^\]]*)\]\(([^)\s]+)\)/g, (_m, alt, src) => `<img src="${src}" alt="${alt}" loading="lazy" />`)
    // links [text](href)
    .replace(/\[([^\]]+)\]\(([^)\s]+)\)/g, (_m, t, href) => {
      const ext = /^https?:\/\//.test(href);
      const rel = ext ? ' target="_blank" rel="noopener noreferrer"' : "";
      return `<a href="${href}"${rel}>${t}</a>`;
    })
    // bold **x** / __x__
    .replace(/(\*\*|__)(?=\S)([\s\S]+?\S)\1/g, "<strong>$2</strong>")
    // italic *x* / _x_
    .replace(/(\*|_)(?=\S)([\s\S]+?\S)\1/g, "<em>$2</em>");

  // restore code spans
  out = out.replace(/\u0000(\d+)\u0000/g, (_m, i) => codes[Number(i)]);
  return out;
}

export function markdownToHtml(md: string): string {
  const lines = md.replace(/\r\n/g, "\n").split("\n");
  const html: string[] = [];
  let i = 0;

  const flushParagraph = (buf: string[]) => {
    if (buf.length) html.push(`<p>${inline(esc(buf.join(" ").trim()))}</p>`);
    buf.length = 0;
  };

  let para: string[] = [];

  while (i < lines.length) {
    const line = lines[i];

    // directive: a lone `:::release-table` (etc.) becomes a sentinel the page
    // hydrates into a live component. Kept out of the escaped text stream.
    const directive = line.match(/^:::(release-table)\s*$/);
    if (directive) {
      flushParagraph(para);
      html.push(`<div data-directive="${directive[1]}"></div>`);
      i++;
      continue;
    }

    // callout block:  :::note | :::tip | :::warn [optional label]
    //   ...body lines...
    // :::
    const callout = line.match(/^:::(note|tip|warn)\s*(.*)$/);
    if (callout && !/^:::(release-table)/.test(line)) {
      flushParagraph(para);
      const type = callout[1];
      const label =
        callout[2].trim() ||
        (type === "warn" ? "Warning" : type === "tip" ? "Tip" : "Note");
      const icon = type === "warn" ? "\u26a0" : type === "tip" ? "\u2726" : "\u2139";
      const inner: string[] = [];
      i++;
      while (i < lines.length && !/^:::\s*$/.test(lines[i])) {
        inner.push(lines[i]);
        i++;
      }
      i++; // closing :::
      const cls = type === "note" ? "note" : `note ${type}`;
      const bodyHtml = inline(esc(inner.join(" ").trim()));
      html.push(
        `<div class="${cls}"><p><span class="label"><span class="note-ico">${icon}</span>${esc(
          label,
        )}</span>${bodyHtml}</p></div>`,
      );
      continue;
    }

    // fenced code block ```lang
    const fence = line.match(/^```(\w*)\s*$/);
    if (fence) {
      flushParagraph(para);
      const code: string[] = [];
      i++;
      while (i < lines.length && !/^```\s*$/.test(lines[i])) {
        code.push(lines[i]);
        i++;
      }
      i++; // closing fence
      const lang = fence[1] ? ` data-lang="${fence[1]}"` : "";
      html.push(`<pre class="code"${lang}><code>${esc(code.join("\n"))}</code></pre>`);
      continue;
    }

    // headings
    const h = line.match(/^(#{1,4})\s+(.*)$/);
    if (h) {
      flushParagraph(para);
      const level = h[1].length;
      const text = inline(esc(h[2].trim()));
      const id = h[2]
        .trim()
        .toLowerCase()
        .replace(/[^\w\s-]/g, "")
        .replace(/\s+/g, "-");
      html.push(`<h${level} id="${id}">${text}</h${level}>`);
      i++;
      continue;
    }

    // horizontal rule
    if (/^(-{3,}|\*{3,}|_{3,})\s*$/.test(line)) {
      flushParagraph(para);
      html.push("<hr />");
      i++;
      continue;
    }

    // blockquote
    if (/^>\s?/.test(line)) {
      flushParagraph(para);
      const quote: string[] = [];
      while (i < lines.length && /^>\s?/.test(lines[i])) {
        quote.push(lines[i].replace(/^>\s?/, ""));
        i++;
      }
      html.push(`<blockquote>${inline(esc(quote.join(" ")))}</blockquote>`);
      continue;
    }

    // table: header row | --- | rows
    if (/^\s*\|.*\|\s*$/.test(line) && i + 1 < lines.length && /^\s*\|[\s:|-]+\|\s*$/.test(lines[i + 1])) {
      flushParagraph(para);
      // Split on `|` but not inside `code` spans and not on escaped `\|`.
      const cells = (row: string) => {
        const inner = row.trim().replace(/^\||\|$/g, "");
        const out: string[] = [];
        let cur = "";
        let inCode = false;
        for (let k = 0; k < inner.length; k++) {
          const ch = inner[k];
          if (ch === "\\" && inner[k + 1] === "|") {
            cur += "|";
            k++;
            continue;
          }
          if (ch === "`") inCode = !inCode;
          if (ch === "|" && !inCode) {
            out.push(cur.trim());
            cur = "";
            continue;
          }
          cur += ch;
        }
        out.push(cur.trim());
        return out;
      };
      const head = cells(line);
      i += 2; // skip header + separator
      const rows: string[][] = [];
      while (i < lines.length && /^\s*\|.*\|\s*$/.test(lines[i])) {
        rows.push(cells(lines[i]));
        i++;
      }
      const thead = `<thead><tr>${head.map((c) => `<th>${inline(esc(c))}</th>`).join("")}</tr></thead>`;
      const tbody = `<tbody>${rows
        .map((r) => `<tr>${r.map((c) => `<td>${inline(esc(c))}</td>`).join("")}</tr>`)
        .join("")}</tbody>`;
      html.push(`<div class="tablewrap"><table>${thead}${tbody}</table></div>`);
      continue;
    }

    // lists (ordered / unordered), allowing nested via 2-space indent
    if (/^\s*([-*+]|\d+\.)\s+/.test(line)) {
      flushParagraph(para);
      const ordered = /^\s*\d+\.\s+/.test(line);
      const tag = ordered ? "ol" : "ul";
      const items: string[] = [];
      const itemRe = ordered ? /^\s*\d+\.\s+(.*)$/ : /^\s*[-*+]\s+(.*)$/;
      const isItem = (l: string) => itemRe.test(l);
      const startsBlock = (l: string) =>
        l.trim() === "" ||
        /^```/.test(l) ||
        /^#{1,4}\s/.test(l) ||
        /^>\s?/.test(l) ||
        /^\s*\|.*\|\s*$/.test(l);
      while (i < lines.length && isItem(lines[i])) {
        const m = lines[i].match(itemRe)!;
        let text = m[1];
        i++;
        // Fold soft-wrapped continuation lines (non-blank, not a new item or
        // block start) into the current item so one bullet stays one <li>.
        while (i < lines.length && !isItem(lines[i]) && !startsBlock(lines[i])) {
          text += " " + lines[i].trim();
          i++;
        }
        items.push(`<li>${inline(esc(text.trim()))}</li>`);
      }
      html.push(`<${tag}>${items.join("")}</${tag}>`);
      continue;
    }

    // blank line ends a paragraph
    if (line.trim() === "") {
      flushParagraph(para);
      i++;
      continue;
    }

    // accumulate paragraph text
    para.push(line);
    i++;
  }
  flushParagraph(para);

  return html.join("\n");
}
