/* eslint-disable @next/next/no-img-element */
"use client";

import { Dialog, DialogContent, DialogTitle } from "@/components/ui/dialog";
import { useGallery } from "@/lib/context/gallery-context";
import { type MediaEntry } from "@/lib/types";
import { VisuallyHidden } from "@radix-ui/react-visually-hidden";
import { useEffect, useRef } from "react";

export default function MediaViewer() {
  const { viewer, media } = useGallery();
  const previousItemRef = useRef<MediaEntry | null>(null);

  useEffect(() => {
    if (viewer.viewerIndex !== null) {
      const item = media[viewer.viewerIndex];
      if (item && item !== previousItemRef.current) {
        previousItemRef.current = item;
      }
    } else {
      previousItemRef.current?.unload();
      previousItemRef.current = null;
    }
  }, [viewer.viewerIndex, media]);

  useEffect(() => {
    if (previousItemRef.current && viewer.viewerIndex === null) {
      previousItemRef.current.unload();
      previousItemRef.current = null;
    }
  }, [viewer.viewerIndex]);

  if (viewer.viewerIndex === null) return null;
  const item = media[viewer.viewerIndex] ?? null;
  if (!item) return null;

  return (
    <Dialog open onOpenChange={viewer.close}>
      <VisuallyHidden>
        <DialogTitle>{item.file.name}</DialogTitle>
      </VisuallyHidden>
      <DialogContent className="flex w-fit !max-w-[90vw] justify-center overflow-hidden bg-black p-0">
        {item.file.type.startsWith("video") ? (
          <video
            src={item.url()}
            controls
            autoPlay
            className="max-h-[90vh] max-w-[90vw]"
          />
        ) : (
          <img
            src={item.url()}
            className="max-h-[90vh] max-w-[90vw]"
            alt="media"
          />
        )}
        <img
          src={item.thumb}
          className="absolute inset-0 -z-10 h-full w-full object-cover blur-2xl"
          alt="thumb"
        />
      </DialogContent>
    </Dialog>
  );
}
