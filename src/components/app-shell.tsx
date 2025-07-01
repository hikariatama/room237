import { useGallery } from "@/lib/context/gallery-context";
import { type ReactNode } from "react";
import { LockOverlay } from "./lock-overlay";

export default function AppShell({ children }: { children: ReactNode }) {
  const { locked } = useGallery();
  return (
    <div className="relative flex min-h-screen select-none">
      {children}
      <LockOverlay locked={locked} />
    </div>
  );
}
