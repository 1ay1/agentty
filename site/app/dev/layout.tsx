import type { ReactNode } from "react";

/**
 * dev.agentty.org owns the whole viewport — no site nav, no footer, no
 * page scroll. The DevTerminal is `position: fixed; inset: 0`, so we just
 * hide the shared chrome and lock the body while this route is mounted.
 */
export default function DevLayout({ children }: { children: ReactNode }) {
  return (
    <>
      <style
        // eslint-disable-next-line react/no-danger
        dangerouslySetInnerHTML={{
          __html: `
            body:has(.devt-root) .nav,
            body:has(.devt-root) .foot,
            body:has(.devt-root) .site-fx { display: none !important; }
            body:has(.devt-root) { overflow: hidden; }
            body:has(.devt-root) main { margin: 0 !important; padding: 0 !important; }
          `,
        }}
      />
      {children}
    </>
  );
}
