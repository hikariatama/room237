import "@/styles/globals.css";
import "react-image-crop/dist/ReactCrop.css";

import { type Metadata } from "next";

export const metadata: Metadata = {
  title: "Room 237",
  description: "Manage your photos offline",
  icons: [
    { rel: "icon", url: "/favicon.ico" },
    { rel: "apple-touch-icon", url: "/apple-touch-icon.png", sizes: "180x180" },
    {
      rel: "icon",
      url: "/favicon-32x32.png",
      type: "image/png",
      sizes: "32x32",
    },
    {
      rel: "icon",
      url: "/favicon-16x16.png",
      type: "image/png",
      sizes: "16x16",
    },
    { rel: "manifest", url: "/site.webmanifest" },
  ],
  metadataBase: new URL(
    process.env.NEXT_PUBLIC_SITE_URL ?? "http://localhost:3000",
  ),
  generator: "Next.js",
  manifest: "/manifest.json",
};

export default function RootLayout({
  children,
}: Readonly<{ children: React.ReactNode }>) {
  return (
    <html lang="en">
      <body>{children}</body>
    </html>
  );
}
