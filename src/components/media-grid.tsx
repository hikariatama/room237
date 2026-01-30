"use client";

import { MediaItem } from "@/components/media-item";
import { FAVORITES_ALBUM_ID, MAX_COLS } from "@/lib/consts";
import { useSortedMedia } from "@/lib/hooks/use-sorted-media";
import { useUpload } from "@/lib/hooks/use-upload";
import { isMedia } from "@/lib/utils";
import { useRoom237 } from "@/lib/stores";
import { cn } from "@/lib/utils";
import { useVirtualizer } from "@tanstack/react-virtual";
import { isEqual } from "lodash";
import { IconLayoutGrid, IconUpload } from "@tabler/icons-react";
import {
  useCallback,
  useEffect,
  useLayoutEffect,
  useMemo,
  useRef,
  useState,
} from "react";
import { useStoreWithEqualityFn } from "zustand/traditional";
import { FavoritesAlbum } from "./favorites-album";
import { useDragDrop } from "@/lib/hooks/use-drag-drop";
import { useI18n } from "@/lib/i18n";

const nameForClipboard = (file: File, idx: number) => {
  const trimmed = file.name?.trim();
  if (trimmed) return trimmed;
  const type = file.type || "";
  const ext = type.startsWith("image/")
    ? (type.split("/")[1] ?? "png")
    : type.startsWith("video/")
      ? (type.split("/")[1] ?? "mp4")
      : "bin";
  return `pasted-${idx}.${ext}`;
};

export default function MediaGrid({
  scrollerRef,
}: {
  scrollerRef: React.RefObject<HTMLDivElement | null>;
}) {
  const [scrollEl, setScrollEl] = useState<HTMLDivElement | null>(null);
  const [scrollSize, setScrollSize] = useState({ width: 0, height: 0 });
  const { uploadFilesToActive } = useUpload();
  const columns = useRoom237((state) => state.columns);
  const layout = useRoom237((state) => state.layout);
  const loadingAlbumId = useRoom237((state) => state.loadingAlbumId);
  const isLoadingAlbum = loadingAlbumId !== null;
  const activeAlbum = useStoreWithEqualityFn(
    useRoom237,
    (state) => {
      if (!state.activeAlbumId) return null;
      const album = state.albumsById[state.activeAlbumId];
      if (!album) return null;
      return {
        albumId: album.albumId,
        path: album.path,
      };
    },
    (a, b) => a?.albumId === b?.albumId && a?.path === b?.path,
  );
  const albumMedia = useStoreWithEqualityFn(
    useRoom237,
    (state) => {
      if (!activeAlbum) return null;
      return state.albumMediasByPath[activeAlbum.path] ?? null;
    },
    (a, b) => {
      if (a === b) return true;
      if (!a || !b) return false;
      if (a.length !== b.length) return false;
      for (let i = 0; i < a.length; i++) {
        if (a[i]?.path !== b[i]?.path) return false;
      }
      return true;
    },
  );
  const { mediaPaths } = useSortedMedia();
  const isUnfocused = useRoom237((state) => state.isUnfocused);
  const albumIds = useStoreWithEqualityFn(
    useRoom237,
    (state) => Object.keys(state.albumsById),
    isEqual,
  );
  const draggedItems = useRoom237((state) => state.draggedItems);
  const { clear: clearDraggedItems } = useDragDrop();
  const activeAlbumIsFavorites = useMemo(() => {
    if (!activeAlbum) return false;
    return activeAlbum.path === FAVORITES_ALBUM_ID;
  }, [activeAlbum]);
  const { t } = useI18n();

  const mediaRows = useMemo(() => {
    if (layout === "default") {
      return Array.from(
        { length: Math.ceil(mediaPaths.length / columns) },
        (_, i) => mediaPaths.slice(i * columns, i * columns + columns),
      );
    }
    if (layout === "masonry") {
      // ? Masonry will use different virtualizer
      return [];
    }
    if (layout === "apple") {
      return Array.from(
        { length: Math.ceil(mediaPaths.length / columns) },
        (_, i) => mediaPaths.slice(i * columns, i * columns + columns),
      );
    }
    return [];
  }, [mediaPaths, columns, layout]);

  const mediaPathsRef = useRef<string[]>([]);
  mediaPathsRef.current = mediaPaths;

  useLayoutEffect(() => {
    let cleanup: (() => void) | null = null;
    let raf: number | null = null;

    const attach = () => {
      const el = scrollerRef.current;
      if (!el) return;
      setScrollEl(el);
      const { width, height } = el.getBoundingClientRect();
      setScrollSize({ width, height });
      const observer = new ResizeObserver(([entry]) => {
        if (!entry) return;
        const { width, height } = entry.contentRect;
        setScrollSize({ width, height });
      });
      observer.observe(el);
      cleanup = () => observer.disconnect();
    };

    attach();
    if (!scrollEl && !cleanup) {
      raf = requestAnimationFrame(attach);
    }

    return () => {
      if (raf !== null) cancelAnimationFrame(raf);
      if (cleanup) cleanup();
    };
  }, [scrollerRef, scrollEl]);

  const gridRowCount = useMemo(
    () => Math.ceil(mediaPaths.length / columns),
    [mediaPaths.length, columns],
  );

  const getGridRowKey = useCallback(
    (rowIndex: number) =>
      mediaPathsRef.current[rowIndex * columns] ?? `row-${rowIndex}`,
    [columns],
  );

  const getMasonryKey = useCallback(
    (index: number) => mediaPathsRef.current[index] ?? `item-${index}`,
    [],
  );

  const gapRem = useMemo(() => (1 - columns / MAX_COLS) * 0.5 + 0.5, [columns]);
  const gapPx = gapRem * 16;
  const gapValue = `${gapRem}rem`;
  const halfGapValue = `${gapRem / 2}rem`;
  const rowEstimate = useMemo(() => {
    const containerWidth =
      scrollSize.width ??
      scrollEl?.clientWidth ??
      (typeof window !== "undefined" ? window.innerWidth : 1200);
    const usableWidth = Math.max(containerWidth - (columns - 1) * gapPx, 0);
    const columnWidth = Math.max(usableWidth / columns, 64);
    return columnWidth + gapPx;
  }, [columns, gapPx, scrollEl?.clientWidth, scrollSize.width]);

  const measureWithFallback = useCallback(
    (el: Element) => {
      const size = el.getBoundingClientRect().height;
      return size > 0 ? size : rowEstimate;
    },
    [rowEstimate],
  );

  const scrollTarget = scrollEl ?? scrollerRef.current;

  const rowVirtualizerGrid = useVirtualizer({
    count: gridRowCount,
    getScrollElement: () => scrollTarget,
    estimateSize: () => rowEstimate,
    overscan: 3,
    getItemKey: getGridRowKey,
    measureElement: measureWithFallback,
  });

  const rowVirtualizerMasonry = useVirtualizer({
    count: mediaPaths.length,
    getScrollElement: () => scrollTarget,
    estimateSize: () => rowEstimate,
    overscan: 3,
    lanes: columns,
    getItemKey: getMasonryKey,
    measureElement: measureWithFallback,
  });

  const scrollerHeight = scrollSize.height ?? scrollTarget?.clientHeight ?? 0;
  const virtualContentHeight =
    layout === "masonry"
      ? rowVirtualizerMasonry.getTotalSize()
      : rowVirtualizerGrid.getTotalSize();
  const dropZoneHeight = Math.max(virtualContentHeight, scrollerHeight || 0);

  const drop = useCallback(
    (e: React.DragEvent<HTMLDivElement>) => {
      e.preventDefault();
      e.stopPropagation();
      const hasFiles = Boolean(e.dataTransfer?.files?.length);
      if (hasFiles) {
        void uploadFilesToActive(e.dataTransfer.files);
        clearDraggedItems();
        return;
      }

      if (draggedItems.length > 0) {
        clearDraggedItems();
      }
    },
    [clearDraggedItems, draggedItems.length, uploadFilesToActive],
  );

  const over = useCallback(
    (e: React.DragEvent<HTMLDivElement>) => {
      e.preventDefault();
      e.stopPropagation();
      const hasFiles = Boolean(e.dataTransfer?.files?.length);
      e.dataTransfer.dropEffect =
        hasFiles || draggedItems.length === 0 ? "copy" : "move";
    },
    [draggedItems.length],
  );

  useEffect(() => {
    const handlePaste = (e: ClipboardEvent) => {
      const target = e.target as HTMLElement | null;
      const isEditable =
        target &&
        (target.tagName === "INPUT" ||
          target.tagName === "TEXTAREA" ||
          target.isContentEditable);

      const files = Array.from(e.clipboardData?.files ?? []);
      if (files.length === 0 || isEditable) {
        return;
      }

      const mediaFiles = files.filter((f, idx) =>
        isMedia(nameForClipboard(f, idx)),
      );
      if (!mediaFiles.length) {
        return;
      }

      e.preventDefault();
      void uploadFilesToActive(mediaFiles).catch((error) => {
        console.error("Paste upload failed", error);
      });
    };
    document.addEventListener("paste", handlePaste);
    return () => {
      document.removeEventListener("paste", handlePaste);
    };
  }, [uploadFilesToActive]);

  const rootDir = useRoom237((state) => state.rootDir);
  const mediaCount = mediaPaths.length;

  if (!activeAlbum && albumIds.length > 0 && rootDir) {
    return (
      <div className="flex h-[90vh] flex-col items-center justify-center pb-4">
        <IconLayoutGrid className="text-muted-foreground mb-3 size-10" />
        <div className="text-muted-foreground mb-1 text-lg">
          {t("media.selectAlbum")}
        </div>
      </div>
    );
  }

  if (
    activeAlbum &&
    !isLoadingAlbum &&
    albumMedia !== null &&
    mediaCount === 0 &&
    albumIds.length > 0
  ) {
    return (
      <div className="flex h-[90vh] flex-col items-center justify-center pb-6">
        <IconUpload className="text-muted-foreground mb-3 size-10" />
        <div className="text-muted-foreground mb-1 text-lg">
          {t("media.empty.title")}
        </div>
        <div className="text-muted-foreground text-sm">
          {t("media.empty.subtitle")}
        </div>
      </div>
    );
  }

  if (isLoadingAlbum) {
    if (layout === "default")
      return (
        <div
          className="grid grid-cols-1 gap-2"
          style={{
            gridTemplateColumns: `repeat(${columns}, 1fr)`,
            gap: gapValue,
          }}
        >
          {Array.from({ length: columns * 5 }, (_, i) => (
            <div
              key={i}
              className="bg-background/40 border-border/50 aspect-square animate-pulse rounded-lg border"
              style={{
                animationDelay: `${(i / (columns * 5 - 1)) * 1.5}s`,
              }}
            />
          ))}
        </div>
      );

    if (layout === "masonry")
      return (
        <div
          className="grid gap-2"
          style={{ gridTemplateColumns: `repeat(${columns}, 1fr)` }}
        >
          {Array.from({ length: columns }, (_, i) => (
            <div key={`col-${i}`} className="flex flex-col">
              {Array.from({ length: 5 }, (_, j) => (
                <div
                  key={`${i}-${j}`}
                  className="bg-background/40 border-border/50 mb-2 w-full animate-pulse rounded-lg border"
                  style={{
                    height: `${Math.random() * 100 + 150}px`,
                    animationDelay: `${((i * 5 + j) / (columns * 5 - 1)) * 1.5}s`,
                  }}
                />
              ))}
            </div>
          ))}
        </div>
      );

    if (layout === "apple")
      return (
        <div
          className="grid gap-4"
          style={{ gridTemplateColumns: `repeat(${columns}, 1fr)` }}
        >
          {Array.from({ length: columns * 5 }, (_, i) => (
            <div
              key={i}
              className="flex aspect-square items-center justify-center"
            >
              {Math.random() > 0.2 ? (
                <div
                  className="bg-background/40 border-border/50 w-full animate-pulse rounded-lg border"
                  style={{
                    height: `${Math.random() * 20 + 50}%`,
                    animationDelay: `${(i / (columns * 5 - 1)) * 1.5}s`,
                  }}
                />
              ) : (
                <div
                  className="bg-background/40 border-border/50 h-full animate-pulse rounded-lg border"
                  style={{
                    width: `${Math.random() * 20 + 50}%`,
                    animationDelay: `${(i / (columns * 5 - 1)) * 1.5}s`,
                  }}
                />
              )}
            </div>
          ))}
        </div>
      );
  }

  if (activeAlbumIsFavorites) {
    return <FavoritesAlbum drop={drop} over={over} />;
  }

  if (!scrollTarget) return null;

  return (
    <div
      className={cn(
        "relative transition-all duration-200 ease-in-out",
        isUnfocused && "overflow-hidden opacity-20 blur-xl",
      )}
      data-allow-drop="true"
      style={{
        height: `${dropZoneHeight}px`,
      }}
      onDragOver={over}
      onDrop={drop}
    >
      {layout === "default" &&
        rowVirtualizerGrid.getVirtualItems().map((virtualRow) => (
          <div
            className="absolute top-0 left-0 grid w-full"
            key={virtualRow.key}
            data-index={virtualRow.index}
            ref={rowVirtualizerGrid.measureElement}
            style={{
              transform: `translateY(${virtualRow.start}px)`,
              gridTemplateColumns: `repeat(${columns}, 1fr)`,
              gap: gapValue,
              paddingBottom: gapValue,
            }}
          >
            {mediaRows[virtualRow.index]!.map((item) => (
              <MediaItem
                key={item}
                mediaPath={item}
                className="m-0 aspect-square w-full object-cover"
                imgClassName="w-full object-cover aspect-square"
              />
            ))}
          </div>
        ))}
      {layout === "masonry" &&
        rowVirtualizerMasonry.getVirtualItems().map((virtualRow) => (
          <div
            key={virtualRow.key}
            data-index={virtualRow.index}
            ref={rowVirtualizerMasonry.measureElement}
            style={{
              transform: `translateY(${virtualRow.start}px)`,
              width: `${100 / columns}%`,
              padding: halfGapValue,
              left: `${(virtualRow.lane * 100) / columns}%`,
            }}
            className="absolute top-0"
          >
            <MediaItem
              mediaPath={mediaPaths[virtualRow.index]!}
              className="m-0 w-full object-cover"
              imgClassName="w-full object-cover"
            />
          </div>
        ))}
      {layout === "apple" &&
        rowVirtualizerGrid.getVirtualItems().map((virtualRow) => (
          <div
            className="absolute top-0 left-0 grid w-full"
            key={virtualRow.key}
            data-index={virtualRow.index}
            ref={rowVirtualizerGrid.measureElement}
            style={{
              transform: `translateY(${virtualRow.start}px)`,
              gridTemplateColumns: `repeat(${columns}, 1fr)`,
              gap: gapValue,
              paddingBottom: gapValue,
            }}
          >
            {mediaRows[virtualRow.index]!.map((item) => (
              <div
                key={item}
                className="flex w-full items-center justify-center"
              >
                <MediaItem mediaPath={item} className="m-0" />
              </div>
            ))}
          </div>
        ))}
    </div>
  );
}
