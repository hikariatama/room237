/* eslint-disable @next/next/no-img-element */
"use client";

import { useState } from "react";
import type { Album } from "@/lib/types";
import { cn } from "@/lib/utils";
import { motion } from "framer-motion";
import { EyeOff } from "lucide-react";
import { type DragEvent as ReactDragEvent } from "react";
import { useGallery } from "@/lib/context/gallery-context";

interface AlbumItemProps {
  album: Album;
  active: boolean;
  onClick: () => void;
}

export const AlbumItem: React.FC<AlbumItemProps> = ({
  album,
  active,
  onClick,
}) => {
  const { addFilesToAlbum, moveDraggedToAlbum } = useGallery();
  const [highlighted, setHighlighted] = useState(false);

  const handleDrop = (e: ReactDragEvent<HTMLDivElement>): void => {
    e.preventDefault();
    setHighlighted(false);
    if (e.dataTransfer?.files?.length) {
      void addFilesToAlbum(album, e.dataTransfer.files);
    } else {
      void moveDraggedToAlbum(album);
    }
  };

  const handleDragEnter = (e: ReactDragEvent<HTMLDivElement>): void => {
    e.preventDefault();
    setHighlighted(true);
  };

  const handleDragLeave = (): void => setHighlighted(false);

  return (
    <motion.div
      exit={{ opacity: 0, scale: 0.8, transition: { duration: 0.1 } }}
      onClick={onClick}
      onDragEnter={handleDragEnter}
      onDragOver={(e) => {
        e.preventDefault();
        setHighlighted(true);
      }}
      onDragLeave={handleDragLeave}
      onDrop={handleDrop as unknown as React.DragEventHandler<HTMLDivElement>}
      className={cn(
        "relative mb-1 flex cursor-pointer items-center gap-2 rounded-lg border-2 py-1 pr-3 pl-2 transition-colors select-none",
        active ? "bg-white/10" : "hover:bg-white/5",
        highlighted ? "border-primary" : "border-transparent",
      )}
    >
      <div className="relative size-7 flex-shrink-0 overflow-hidden rounded-md bg-white/10">
        {album.thumb && (
          <img
            src={album.thumb}
            alt="thumb"
            className="h-full w-full object-cover blur-[1px]"
          />
        )}
        <div className="absolute inset-0 flex items-center justify-center bg-black/40">
          <EyeOff className="text-foreground size-3 opacity-70" />
        </div>
      </div>
      <span className="flex-1 truncate text-sm">{album.name}</span>
      <span className="text-muted-foreground text-sm">
        {album.images.length}
      </span>
    </motion.div>
  );
};
