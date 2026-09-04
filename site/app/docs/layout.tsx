import { DocsSidebar } from "@/components/DocsSidebar";
import { TableOfContents } from "@/components/TableOfContents";

export default function DocsLayout({
  children,
}: Readonly<{ children: React.ReactNode }>) {
  return (
    <div className="docs-shell">
      <DocsSidebar />
      <article className="docs-main">{children}</article>
      <TableOfContents />
    </div>
  );
}
