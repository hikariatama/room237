"use client";

import { useEffect, useMemo, useRef } from "react";
import { useGallery } from "@/lib/context/gallery-context";
import { masonry } from "@/lib/utils";
import { MasonryMedia } from "@/components/masonry-media";

export default function MasonryGrid() {
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

  return (
    <div
      className="grid gap-4"
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
            <MasonryMedia
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
}
