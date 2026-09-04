import { ImageResponse } from "next/og";
import { getAllPosts, getPost, formatDate } from "@/lib/blog";

export const dynamic = "force-static";
export const size = { width: 1200, height: 630 };
export const contentType = "image/png";

// One social card per post, rendered at build time. Title + date + reading
// time on the same brand surface as the site OG image, so a blog link unfurls
// with the actual headline instead of the generic homepage card.
export function generateStaticParams() {
  return getAllPosts().map((p) => ({ slug: p.slug }));
}

export const alt = "agentty blog post";

export default async function Image({
  params,
}: {
  params: Promise<{ slug: string }>;
}) {
  const { slug } = await params;
  const post = getPost(slug);
  const title = post?.title ?? "agentty";
  const meta = post
    ? `${formatDate(post.date)}  ·  ${post.readingMinutes} min read`
    : "";
  const tags = post?.tags ?? [];

  return new ImageResponse(
    (
      <div
        style={{
          width: "100%",
          height: "100%",
          display: "flex",
          flexDirection: "column",
          justifyContent: "space-between",
          padding: "76px 80px",
          backgroundColor: "#0b0810",
          backgroundImage:
            "radial-gradient(1000px 560px at 84% -12%, rgba(217, 70, 239,0.28), transparent 62%), radial-gradient(720px 460px at 6% 110%, rgba(45, 212, 212,0.16), transparent 58%)",
          fontFamily: "monospace",
        }}
      >
        <div
          style={{
            display: "flex",
            alignItems: "center",
            gap: "16px",
            fontSize: "30px",
            fontWeight: 700,
            letterSpacing: "-0.02em",
          }}
        >
          <span style={{ color: "#d946ef" }}>▌</span>
          <span style={{ color: "#f2ecfb" }}>agentty</span>
          <span style={{ color: "#7a6e91", fontSize: "24px", fontWeight: 500 }}>
            / blog
          </span>
        </div>

        <div
          style={{
            display: "flex",
            flexDirection: "column",
            gap: "22px",
          }}
        >
          <div style={{ fontSize: "22px", color: "#2dd4d4", letterSpacing: "0.02em" }}>
            {meta}
          </div>
          <div
            style={{
              fontSize: title.length > 60 ? "58px" : "70px",
              fontWeight: 800,
              lineHeight: 1.06,
              letterSpacing: "-0.035em",
              color: "#f2ecfb",
              maxWidth: "1040px",
              display: "flex",
            }}
          >
            {title}
          </div>
        </div>

        <div style={{ display: "flex", alignItems: "center", gap: "12px" }}>
          {tags.slice(0, 4).map((t) => (
            <div
              key={t}
              style={{
                fontSize: "22px",
                color: "#ea8bfb",
                border: "1px solid rgba(217, 70, 239,0.4)",
                borderRadius: "999px",
                padding: "6px 20px",
                display: "flex",
              }}
            >
              {t}
            </div>
          ))}
        </div>
      </div>
    ),
    { ...size },
  );
}
