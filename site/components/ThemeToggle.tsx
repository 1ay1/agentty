"use client";

import { useEffect, useState } from "react";

type Theme = "dark" | "light";

export function ThemeToggle() {
  const [theme, setTheme] = useState<Theme>("dark");
  const [mounted, setMounted] = useState(false);

  useEffect(() => {
    const current =
      (document.documentElement.getAttribute("data-theme") as Theme) || "dark";
    setTheme(current);
    setMounted(true);
  }, []);

  const toggle = () => {
    const next: Theme = theme === "dark" ? "light" : "dark";
    document.documentElement.setAttribute("data-theme", next);
    try {
      localStorage.setItem("theme", next);
    } catch {}
    setTheme(next);
  };

  // Avoid hydration mismatch: render a neutral button until mounted.
  const isLight = mounted && theme === "light";

  return (
    <button
      className="theme-toggle"
      onClick={toggle}
      aria-label={isLight ? "Switch to dark theme" : "Switch to light theme"}
      title={isLight ? "Switch to dark theme" : "Switch to light theme"}
    >
      <svg
        className="theme-icon sun"
        viewBox="0 0 24 24"
        width="17"
        height="17"
        aria-hidden="true"
      >
        <circle cx="12" cy="12" r="4.2" fill="currentColor" />
        <g stroke="currentColor" strokeWidth="1.8" strokeLinecap="round">
          <line x1="12" y1="2.5" x2="12" y2="5" />
          <line x1="12" y1="19" x2="12" y2="21.5" />
          <line x1="2.5" y1="12" x2="5" y2="12" />
          <line x1="19" y1="12" x2="21.5" y2="12" />
          <line x1="5.2" y1="5.2" x2="6.9" y2="6.9" />
          <line x1="17.1" y1="17.1" x2="18.8" y2="18.8" />
          <line x1="5.2" y1="18.8" x2="6.9" y2="17.1" />
          <line x1="17.1" y1="6.9" x2="18.8" y2="5.2" />
        </g>
      </svg>
      <svg
        className="theme-icon moon"
        viewBox="0 0 24 24"
        width="17"
        height="17"
        aria-hidden="true"
      >
        <path
          fill="currentColor"
          d="M21 12.79A9 9 0 1 1 11.21 3a7 7 0 0 0 9.79 9.79z"
        />
      </svg>
    </button>
  );
}
