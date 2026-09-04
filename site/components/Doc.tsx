"use client";

import { useState } from "react";

export function Code({
  children,
  filename,
  lang,
}: {
  children: string;
  filename?: string;
  lang?: string;
}) {
  const [copied, setCopied] = useState(false);
  return (
    <div className="codeblock">
      {(filename || lang) && (
        <div className="codeblock-head">
          <span className="codeblock-name">{filename ?? lang}</span>
          <button
            className="codeblock-copy"
            aria-label="Copy code"
            onClick={() => {
              navigator.clipboard?.writeText(children);
              setCopied(true);
              setTimeout(() => setCopied(false), 1400);
            }}
          >
            {copied ? "copied ✓" : "copy"}
          </button>
        </div>
      )}
      <pre className="code">
        <code>{children}</code>
        {!filename && !lang && (
          <button
            className="codeblock-copy floating"
            aria-label="Copy code"
            onClick={() => {
              navigator.clipboard?.writeText(children);
              setCopied(true);
              setTimeout(() => setCopied(false), 1400);
            }}
          >
            {copied ? "✓" : "⧉"}
          </button>
        )}
      </pre>
    </div>
  );
}

export function Note({
  type = "note",
  label,
  children,
}: {
  type?: "note" | "warn" | "tip";
  label?: string;
  children: React.ReactNode;
}) {
  const lbl = label ?? (type === "warn" ? "Warning" : type === "tip" ? "Tip" : "Note");
  const icon = type === "warn" ? "⚠" : type === "tip" ? "✦" : "ℹ";
  return (
    <div className={`note ${type === "note" ? "" : type}`}>
      <p>
        <span className="label"><span className="note-ico">{icon}</span>{lbl}</span>
        {children}
      </p>
    </div>
  );
}
