"use client";

import { useEffect, useMemo, useRef } from "react";
import { useGallery } from "@/lib/context/gallery-context";
import { masonry } from "@/lib/utils";
import { MediaItem } from "@/components/media-item";

const MAX_COLS = 12;

export default function MediaGrid() {
  const {
    media,
    selection,
    toggleSelect,
    viewer,
    loadMore,
    onDragStart,
    uploadFilesToActive,
    deleteMedia,
    columns,
    layout,
  } = useGallery();

  const cols = useMemo(() => masonry(media, columns), [media, columns]);
  const sent = useRef<HTMLDivElement[]>([]);

  useEffect(() => {
    const obs = new IntersectionObserver(
      ([e]) => {
        if (e?.isIntersecting) loadMore();
      },
      { rootMargin: `${window.innerHeight * 5}px` },
    );
    sent.current.forEach((el) => obs.observe(el));
    return () => obs.disconnect();
  }, [media, loadMore]);

  const drop = (e: React.DragEvent<HTMLDivElement>) => {
    e.preventDefault();
    if (e.dataTransfer?.files?.length)
      void uploadFilesToActive(e.dataTransfer.files);
  };
  const over = (e: React.DragEvent<HTMLDivElement>) => {
    e.preventDefault();
    e.dataTransfer.dropEffect = "copy";
  };

  useEffect(() => {
    const handlePaste = (e: ClipboardEvent) => {
      e.preventDefault();
      if (e.clipboardData?.files.length) {
        void uploadFilesToActive(e.clipboardData.files);
      }
    };
    document.addEventListener("paste", handlePaste);
    return () => {
      document.removeEventListener("paste", handlePaste);
    };
  }, [uploadFilesToActive]);

  if (layout === "default")
    return (
      <div
        className="grid transition-all duration-200 ease-in-out"
        style={{
          gridTemplateColumns: `repeat(${columns}, 1fr)`,
          gap: `${(1 - columns / MAX_COLS) * 0.5 + 0.5}rem`,
        }}
        onDragOver={over}
        onDrop={drop}
      >
        {media.map((img) => (
          <MediaItem
            key={img.file.name}
            item={img}
            selected={selection.has(img)}
            onSelectToggle={toggleSelect}
            onDragStart={onDragStart}
            onView={() =>
              viewer.open(media.findIndex((p) => p.file.name === img.file.name))
            }
            onRequestDelete={deleteMedia}
            className="m-0 aspect-square w-full object-cover"
            imgClassName="w-full h-full object-cover"
            showExtras={columns < 10}
            style={{
              borderRadius: `${(1 - columns / MAX_COLS) * 0.75 + 0.15}rem`,
              fontSize: `${(1 - columns / MAX_COLS) * 4 + 8}px`,
            }}
          />
        ))}
        <div
          className="col-span-full"
          ref={(el) => {
            if (el) sent.current[0] = el;
          }}
          style={{ height: 1 }}
        />
      </div>
    );

  if (layout === "masonry")
    return (
      <div
        className="grid gap-2 transition-all duration-200 ease-in-out"
        style={{ gridTemplateColumns: `repeat(${cols.length}, 1fr)` }}
        onDragOver={over}
        onDrop={drop}
      >
        {cols.map((col, i) => (
          <div
            key={`col-${i}`}
            className="flex flex-col"
            onDragOver={over}
            onDrop={drop}
          >
            {col.map((img) => (
              <MediaItem
                key={img.file.name}
                item={img}
                selected={selection.has(img)}
                onSelectToggle={toggleSelect}
                onDragStart={onDragStart}
                onView={() =>
                  viewer.open(
                    media.findIndex((p) => p.file.name === img.file.name),
                  )
                }
                onRequestDelete={deleteMedia}
                showExtras={columns < 8}
              />
            ))}
            <div
              ref={(el) => {
                if (el) sent.current[i] = el;
              }}
              style={{ height: 1 }}
            />
          </div>
        ))}
      </div>
    );

  if (layout === "apple")
    return (
      <div
        className="grid gap-4 transition-all duration-200 ease-in-out"
        style={{ gridTemplateColumns: `repeat(${columns}, 1fr)` }}
        onDragOver={over}
        onDrop={drop}
      >
        {media.map((img) => (
          <div key={img.file.name} className="flex items-center justify-center">
            <MediaItem
              item={img}
              selected={selection.has(img)}
              onSelectToggle={toggleSelect}
              onDragStart={onDragStart}
              onView={() =>
                viewer.open(
                  media.findIndex((p) => p.file.name === img.file.name),
                )
              }
              onRequestDelete={deleteMedia}
              showExtras={columns < 8}
            />
          </div>
        ))}
        <div
          className="col-span-full"
          ref={(el) => {
            if (el) sent.current[0] = el;
          }}
          style={{ height: 1 }}
        />
      </div>
    );
}
