import type { Metadata } from "next";
import { ContentPageView, renderContentMetadata } from "@/components/ContentPageView";
export const metadata: Metadata = renderContentMetadata("security");
export default function Page() { return <ContentPageView slug="security" />; }
