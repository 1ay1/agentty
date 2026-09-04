// Thin typed wrapper around the self-hosted Umami tracker.
//
// Umami injects a global `umami` object (see app/layout.tsx). This helper
// safely no-ops when the script hasn't loaded (SSR, ad-blocked, or offline)
// so call sites never need to guard.

declare global {
  interface Window {
    umami?: {
      track: (
        event: string | ((props: Record<string, unknown>) => Record<string, unknown>),
        data?: Record<string, unknown>,
      ) => void;
    };
  }
}

/** Fire a named custom event with optional structured data. */
export function track(event: string, data?: Record<string, unknown>): void {
  if (typeof window === "undefined") return;
  try {
    window.umami?.track(event, data);
  } catch {
    /* analytics must never break the UI */
  }
}
