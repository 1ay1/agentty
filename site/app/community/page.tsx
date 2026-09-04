import type { Metadata } from "next";
import { ContentPageView, renderContentMetadata } from "@/components/ContentPageView";
export const metadata: Metadata = renderContentMetadata("community");
export default function Page() { return <ContentPageView slug="community" />; }
