import type { Metadata } from "next";
import { ContentPageView, renderContentMetadata } from "@/components/ContentPageView";
export const metadata: Metadata = renderContentMetadata("code-of-conduct");
export default function Page() { return <ContentPageView slug="code-of-conduct" />; }
