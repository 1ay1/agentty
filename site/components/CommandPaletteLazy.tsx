"use client";

import { useEffect, useState } from "react";
import dynamic from "next/dynamic";

// The palette itself (with useRouter + the search index) is only loaded the
// first time the user actually summons it — keeping it out of the initial
// hydration bundle entirely. Until then this component ships ~nothing.
const Palette = dynamic(
  () => import("./CommandPalette").then((m) => m.CommandPalette),
  { ssr: false },
);

export function CommandPaletteLazy() {
  const [armed, setArmed] = useState(false);

  useEffect(() => {
    if (armed) return;
    const arm = () => setArmed(true);
    const onKey = (e: KeyboardEvent) => {
      if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === "k") {
        e.preventDefault();
        arm();
      }
    };
    // Arm on the first ⌘K, or on the first user interaction with the trigger.
    window.addEventListener("keydown", onKey);
    window.addEventListener("agentty:open-palette", arm as EventListener);
    return () => {
      window.removeEventListener("keydown", onKey);
      window.removeEventListener("agentty:open-palette", arm as EventListener);
    };
  }, [armed]);

  if (!armed) return null;
  // Once armed, the real palette mounts and immediately opens (it re-reads the
  // ⌘K listener itself). We pass a prop so it knows to start open.
  return <Palette startOpen />;
}
