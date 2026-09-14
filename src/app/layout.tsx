import type { Metadata, Viewport } from "next";
import { SessionProviderClient } from "@/components/SessionProviderClient";
import "./globals.css";

export const metadata: Metadata = {
  title: "ESP32 Console",
  description: "Flash ESP32 boards over USB and configure linked devices from the browser.",
};

export const viewport: Viewport = {
  width: "device-width",
  initialScale: 1,
  viewportFit: "cover",
  themeColor: [
    { media: "(prefers-color-scheme: light)", color: "#fafafa" },
    { media: "(prefers-color-scheme: dark)", color: "#09090b" },
  ],
};

export default function RootLayout({ children }: LayoutProps<"/">) {
  return (
    <html
      lang="en"
      className="h-full antialiased"
    >
      <body className="min-h-full bg-zinc-50 text-zinc-900 dark:bg-zinc-950 dark:text-zinc-100">
        <SessionProviderClient>{children}</SessionProviderClient>
      </body>
    </html>
  );
}
