"use client";

import AppShell from "@/components/app-shell";
import DirectoryPicker from "@/components/sidebar/directory-picker";
import AlbumList from "@/components/sidebar/album-list";
import MediaGrid from "@/components/media-grid";
import MediaGridHeader from "@/components/ui/media-grid-header";
import MediaViewer from "@/components/media-viewer";
import { GalleryProvider } from "@/lib/context/gallery-provider";
import { Toaster } from "sonner";
import { SelectionMenu } from "@/components/selection-menu";

export default function GalleryPage() {
  return (
    <GalleryProvider>
      <AppShell>
        <div className="bg-background/95 border-border w-64 space-y-3 border-r p-4">
          <DirectoryPicker />
          <AlbumList />
        </div>
        <div className="h-screen flex-1 overflow-y-auto p-4">
          <MediaGridHeader />
          <MediaGrid />
        </div>
        <MediaViewer />
        <Toaster theme="dark" />
        <SelectionMenu />
      </AppShell>
    </GalleryProvider>
  );
}
