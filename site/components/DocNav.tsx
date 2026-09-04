import Link from "next/link";

// Prev/next footer navigation. The dynamic docs route computes prev/next from
// the frontmatter-derived order (lib/docs.ts) and passes them in directly.
export function DocNav({
  prev,
  next,
}: {
  prev?: { title: string; href: string } | null;
  next?: { title: string; href: string } | null;
}) {
  return (
    <div className="docnav">
      {prev ? (
        <Link href={prev.href}>
          <span className="dir">← Previous</span>
          {prev.title}
        </Link>
      ) : (
        <span style={{ flex: 1 }} />
      )}
      {next ? (
        <Link href={next.href} className="next">
          <span className="dir">Next →</span>
          {next.title}
        </Link>
      ) : (
        <span style={{ flex: 1 }} />
      )}
    </div>
  );
}
