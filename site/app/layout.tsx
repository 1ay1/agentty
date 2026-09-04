import type { Metadata } from "next";
import Script from "next/script";
import { Space_Grotesk, JetBrains_Mono } from "next/font/google";
import "./globals.css";
import { SiteNav } from "@/components/SiteNav";
import { SiteFooter } from "@/components/SiteFooter";
import { DeferredFX } from "@/components/DeferredFX";
import { CommandPaletteLazy } from "@/components/CommandPaletteLazy";
import { site } from "@/lib/site";
import { stats } from "@/lib/stats";

const inter = Space_Grotesk({
  subsets: ["latin"],
  display: "swap",
  variable: "--font-sans",
  // Space Grotesk: a geometric grotesk with a monospace-adjacent, technical
  // character — deliberately NOT Inter, to escape the default AI-SaaS look.
  fallback: ["system-ui", "-apple-system", "Segoe UI", "Roboto", "sans-serif"],
  adjustFontFallback: true,
});

const jetbrainsMono = JetBrains_Mono({
  subsets: ["latin"],
  display: "swap",
  variable: "--font-mono",
  fallback: ["ui-monospace", "SFMono-Regular", "Menlo", "Consolas", "monospace"],
  adjustFontFallback: true,
});

export const viewport = {
  width: "device-width",
  initialScale: 1,
  // Match the mobile browser chrome to the active theme.
  themeColor: [
    { media: "(prefers-color-scheme: dark)", color: "#0b0810" },
    { media: "(prefers-color-scheme: light)", color: "#ffffff" },
  ],
};

export const metadata: Metadata = {
  metadataBase: new URL(site.url),
  title: {
    default: "agentty — a blazing-fast coding agent in your terminal",
    template: "%s · agentty",
  },
  description: site.description,
  applicationName: "agentty",
  generator: "Next.js",
  referrer: "strict-origin-when-cross-origin",
  category: "technology",
  keywords: [
    "agentty",
    "claude code",
    "claude code alternative",
    "claude cli",
    "claude terminal",
    "terminal coding agent",
    "terminal AI agent",
    "AI coding agent",
    "AI pair programmer",
    "command line AI",
    "cli coding assistant",
    "anthropic claude cli",
    "C++26",
    "native coding agent",
    "static binary AI agent",
    "sandboxed coding agent",
    "ssh air-gap agent",
    "aider alternative",
    "open source claude code",
    "no node ai cli",
  ],
  authors: [{ name: "agentty contributors", url: site.github }],
  creator: "agentty contributors",
  publisher: "agentty",
  alternates: {
    canonical: "/",
  },
  openGraph: {
    type: "website",
    url: site.url,
    title: "agentty — a blazing-fast coding agent in your terminal",
    description: site.description,
    siteName: "agentty",
    locale: "en_US",
  },
  twitter: {
    card: "summary_large_image",
    title: "agentty — a blazing-fast coding agent in your terminal",
    description: site.description,
    creator: "@agentty",
  },
  robots: {
    index: true,
    follow: true,
    googleBot: {
      index: true,
      follow: true,
      "max-image-preview": "large",
      "max-snippet": -1,
      "max-video-preview": -1,
    },
  },
  icons: {
    icon: [
      { url: "/favicon.svg", type: "image/svg+xml" },
    ],
    shortcut: "/favicon.svg",
    apple: "/favicon.svg",
  },
  manifest: "/manifest.webmanifest",
  verification: {
    google: "7TjyM5swytiizAv-slWHVDdAEvklDKYmLQMGNqHkCJw",
  },
};

const jsonLd = {
  "@context": "https://schema.org",
  "@graph": [
    {
      "@type": "SoftwareApplication",
      "@id": `${site.url}/#software`,
      name: "agentty",
      alternateName: ["agent++", "agentty cli"],
      applicationCategory: "DeveloperApplication",
      applicationSubCategory: "AI Coding Agent",
      operatingSystem: "Linux, macOS, Windows",
      description: site.description,
      url: site.url,
      downloadUrl: site.releasesLatest,
      softwareVersion: stats.version,
      license: "https://opensource.org/licenses/MIT",
      programmingLanguage: "C++",
      isAccessibleForFree: true,
      offers: { "@type": "Offer", price: "0", priceCurrency: "USD" },
      keywords:
        "claude code alternative, terminal coding agent, claude cli, AI coding agent, C++26",
      author: { "@id": `${site.url}/#org` },
    },
    {
      "@type": "WebSite",
      "@id": `${site.url}/#website`,
      url: site.url,
      name: "agentty",
      description: site.tagline,
      publisher: { "@id": `${site.url}/#org` },
      potentialAction: {
        "@type": "SearchAction",
        target: {
          "@type": "EntryPoint",
          urlTemplate: `${site.url}/docs?q={search_term_string}`,
        },
        "query-input": "required name=search_term_string",
      },
    },
    {
      "@type": "Organization",
      "@id": `${site.url}/#org`,
      name: "agentty",
      url: site.url,
      logo: `${site.url}/favicon.svg`,
      sameAs: [site.github],
    },
  ],
};

export default function RootLayout({
  children,
}: Readonly<{ children: React.ReactNode }>) {
  return (
    <html lang="en" className={`${inter.variable} ${jetbrainsMono.variable}`} suppressHydrationWarning>
      <head>
        {/* Set the theme before first paint to avoid a flash. Priority:
            explicit user choice (localStorage) → OS prefers-color-scheme →
            dark default. Kept as a tiny blocking script so there's no FOUC. */}
        <script
          dangerouslySetInnerHTML={{
            __html: `(function(){try{var t=localStorage.getItem('theme');if(t!=='light'&&t!=='dark'){t=(window.matchMedia&&window.matchMedia('(prefers-color-scheme: light)').matches)?'light':'dark';}document.documentElement.setAttribute('data-theme',t);}catch(e){document.documentElement.setAttribute('data-theme','dark');}})();`,
          }}
        />
        <script
          type="application/ld+json"
          dangerouslySetInnerHTML={{ __html: JSON.stringify(jsonLd) }}
        />
        {/* Self-hosted Umami analytics — first-party, privacy-friendly, no
            cookies, served from our own subdomain so ad-blockers that target
            third-party trackers leave it alone. */}
        <Script
          src="https://analytics.agentty.org/script.js"
          data-website-id="4dc82793-234c-474c-837f-8bc736d6b954"
          strategy="afterInteractive"
          defer
        />
      </head>
      <body>
        <a className="skip" href="#main">Skip to content</a>
        <SiteNav />
        <main id="main">{children}</main>
        <SiteFooter />
        <DeferredFX />
        <CommandPaletteLazy />
      </body>
    </html>
  );
}
