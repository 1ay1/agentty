import type { Metadata } from "next";
import { DevTerminalMount } from "@/components/DevTerminalMount";

export const metadata: Metadata = {
  title: "agentty — live terminal",
  description:
    "Drive a live agentty session in your browser. Type prompts, run slash commands, watch tool calls stream and the token/sec meter tick — a faithful replica of the real C++26 terminal agent.",
  robots: { index: false, follow: false },
};

export default function DevPage() {
  return <DevTerminalMount />;
}
