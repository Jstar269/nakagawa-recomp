import type { Metadata } from "next";
import "./globals.css";
import { Toaster } from "@/components/ui/toaster";

export const metadata: Metadata = {
  title: "Nakagawa Recomp — PSP static recompiler",
  description:
    "Local build, run, diagnostics, and boot-health dashboard for the HST PSP static recompiler.",
  keywords: [
    "PSP",
    "recompiler",
    "Hot Shots Tennis",
    "Get a Grip",
    "Everybody's Tennis",
    "Minna no Tennis",
    "Clap Hanz",
    "PPSSPP",
    "static recompilation",
    "compatibility research",
  ],
  authors: [{ name: "Nakagawa Recomp contributors" }],
};

// System font stack — works fully offline with no external requests.
const SYSTEM_SANS = [
  "system-ui",
  "-apple-system",
  "BlinkMacSystemFont",
  "Segoe UI",
  "Roboto",
  "Helvetica Neue",
  "Arial",
  "sans-serif",
].join(", ");

const SYSTEM_MONO = [
  "ui-monospace",
  "SFMono-Regular",
  "SF Mono",
  "Menlo",
  "Consolas",
  "Liberation Mono",
  "monospace",
].join(", ");

export default function RootLayout({
  children,
}: Readonly<{
  children: React.ReactNode;
}>) {
  return (
    <html lang="en" suppressHydrationWarning>
      <head>
        <style>{`
          :root {
            --font-geist-sans: ${SYSTEM_SANS};
            --font-geist-mono: ${SYSTEM_MONO};
          }
        `}</style>
        <script
          dangerouslySetInnerHTML={{
            __html: `(function(){try{var t=localStorage.getItem('hst-theme');if(!t||t==='dark'){document.documentElement.classList.add('dark');}}catch(e){document.documentElement.classList.add('dark');}})();`,
          }}
        />
      </head>
      <body
        className="antialiased bg-background text-foreground min-h-screen"
        style={{ fontFamily: SYSTEM_SANS }}
      >
        <a
          href="#main-content"
          className="sr-only fixed left-3 top-3 z-[200] rounded-md bg-primary px-3 py-2 text-sm font-semibold text-primary-foreground shadow-lg focus:not-sr-only focus:outline-none focus:ring-2 focus:ring-ring"
        >
          Skip to main content
        </a>
        {children}
        <Toaster />
      </body>
    </html>
  );
}
