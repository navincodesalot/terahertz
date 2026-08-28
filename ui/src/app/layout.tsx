import "@/styles/globals.css";

import { type Metadata } from "next";
import { GeistMono } from "geist/font/mono";
import { GeistSans } from "geist/font/sans";

import { Toaster } from "@/components/ui/sonner";

export const metadata: Metadata = {
  title: "Terahertz | Free-Space Optical Link",
  description: "Web-controlled real-time optical communications dashboard",
  icons: [{ rel: "icon", url: "/favicon.ico" }],
};

export default function RootLayout({
  children,
}: Readonly<{ children: React.ReactNode }>) {
  return (
    <html
      lang="en"
      className={`${GeistSans.className} ${GeistSans.variable} ${GeistMono.variable} dark`}
    >
      <body className="font-sans" suppressHydrationWarning>
        {children}
        <Toaster />
      </body>
    </html>
  );
}
