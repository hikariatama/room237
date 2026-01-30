"use client";

import { useRoom237 } from "@/lib/stores";
import { cn } from "@/lib/utils";
import { AlbumActions } from "./media-grid-header/album-actions";
import { ColumnsSlider } from "./media-grid-header/columns-slider";
import { FavoritesFilter } from "./media-grid-header/favorites-filter";
import { LayoutSelector } from "./media-grid-header/layout-selector";
import { SortDirection } from "./media-grid-header/sort-direction";
import { SortSelector } from "./media-grid-header/sort-selector";

export default function MediaGridHeader() {
  const rootDir = useRoom237((state) => state.rootDir);
  const enabled = useRoom237((state) => !!state.activeAlbumId);

  if (!rootDir) {
    return null;
  }

  return (
    <div className="bg-background/50 border-border/50 shadow-background/20 flex shrink-0 items-center justify-between rounded-3xl border p-1 shadow-lg backdrop-blur-lg">
      <div
        className={cn(
          "flex items-center gap-2",
          !enabled && "pointer-events-none opacity-50",
        )}
      >
        <LayoutSelector />
        <SortSelector />
        <FavoritesFilter />
        <SortDirection />
        <ColumnsSlider />
      </div>
      <div data-tauri-drag-region className="h-full flex-1" />
      <AlbumActions />
    </div>
  );
}
