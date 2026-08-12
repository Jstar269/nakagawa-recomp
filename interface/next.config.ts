import { dirname } from "node:path";
import { fileURLToPath } from "node:url";
import type { NextConfig } from "next";

const projectDir = dirname(fileURLToPath(import.meta.url));

const nextConfig: NextConfig = {
  output: "standalone",
  // Constrain standalone file tracing to the interface app directory so the
  // build does not copy the entire monorepo into .next/standalone.
  outputFileTracingRoot: projectDir,
  outputFileTracingExcludes: {
    "/*": [".env*", "db/**", "tool-results/**", "*.tsbuildinfo"],
    "/api/**/*": [".env*", "db/**", "tool-results/**", "src/**", "*.tsbuildinfo"],
  },
  reactStrictMode: false,
  // TypeScript 7 (Go-native) does not expose the compiler API Next.js uses for
  // its in-process type-checking step; Next recommends useTypeScriptCli so the
  // build shells out to the tsc CLI instead. See the 2026-08 toolchain baseline.
  experimental: {
    useTypeScriptCli: true,
  },
};

export default nextConfig;
