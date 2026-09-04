import Link from "next/link";

export default function NotFound() {
  return (
    <div className="page" style={{ textAlign: "center", paddingTop: 100 }}>
      <h1 style={{ fontSize: 80, margin: 0, fontFamily: "var(--mono)", color: "var(--accent)" }}>404</h1>
      <p className="lead" style={{ margin: "12px 0 28px" }}>
        That page doesn&apos;t exist — like the runtime dependencies agentty doesn&apos;t need.
      </p>
      <Link className="btn btn-primary" href="/">Back home</Link>
    </div>
  );
}
