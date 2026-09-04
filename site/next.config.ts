import type { NextConfig } from "next";

const nextConfig: NextConfig = {
  output: "export",
  trailingSlash: true,
  images: { unoptimized: true },
  reactStrictMode: true,
  productionBrowserSourceMaps: false,
  compiler: {
    // strip console.* from the production bundle
    removeConsole: { exclude: ["error", "warn"] },
  },
  experimental: {
    // inline the critical CSS for first paint, lazy-load the rest
    optimizeCss: true,
  },
};

export default nextConfig;
