/* eslint-disable @next/next/no-img-element */
"use client";

import { Dialog, DialogContent, DialogTitle } from "@/components/ui/dialog";
import { useGallery } from "@/lib/context/gallery-context";
import { type MediaEntry } from "@/lib/types";
import { VisuallyHidden } from "@radix-ui/react-visually-hidden";
import { useEffect, useRef, useState } from "react";
import ReactCrop, { type PercentCrop } from "react-image-crop";
import { Button } from "./ui/button";
import { imgThumb } from "@/lib/hooks/use-upload";
import { cn } from "@/lib/utils";

export default function MediaViewer() {
  const { viewer, media, activeAlbum } = useGallery();
  const previousItemRef = useRef<MediaEntry | null>(null);
  const [crop, setCrop] = useState<PercentCrop | undefined>(undefined);
  const [isEdit, setIsEdit] = useState(false);

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
  const cropAndWrite = async () => {
    if (!item || !crop) return;
    const canvas = document.createElement("canvas");
    const img = new Image();
    img.src = item.url();
    await img.decode();

    const ctx = canvas.getContext("2d");
    if (!ctx) return;

    const pixelRatio = window.devicePixelRatio;

    const cropX = (crop.x / 100) * img.naturalWidth;
    const cropY = (crop.y / 100) * img.naturalHeight;
    const cropWidth = (crop.width / 100) * img.naturalWidth;
    const cropHeight = (crop.height / 100) * img.naturalHeight;

    canvas.width = Math.floor(cropWidth * pixelRatio);
    canvas.height = Math.floor(cropHeight * pixelRatio);

    ctx.scale(pixelRatio, pixelRatio);
    ctx.imageSmoothingQuality = "high";

    ctx.drawImage(
      img,
      cropX,
      cropY,
      cropWidth,
      cropHeight,
      0,
      0,
      cropWidth,
      cropHeight,
    );

    const blob = await new Promise<Blob | null>((resolve) => {
      canvas.toBlob((b) => resolve(b), item.file.type);
    });
    if (!blob) return;
    const newFile = new File([blob], item.file.name, {
      type: item.file.type,
      lastModified: Date.now(),
    });
    const handle = await activeAlbum?.handle.getFileHandle(item.file.name, {
      create: false,
    });
    if (handle) {
      const writable = await handle.createWritable();
      await writable.write(newFile);
      await writable.close();
      item.file = newFile;
      item.thumb = URL.createObjectURL(newFile);
      item.url = () => URL.createObjectURL(newFile);
      item.unload();
      item.unload = () => URL.revokeObjectURL(item.url());
    }
    const thumbDir = await activeAlbum?.handle.getDirectoryHandle(
      ".room237-thumb",
      {
        create: true,
      },
    );
    if (thumbDir) {
      const thumbHandle = await thumbDir.getFileHandle(
        `${item.file.name.replace(/\.[^.]+$/, "")}.avif`,
        { create: true },
      );
      const thumbBlob = await imgThumb(newFile);
      const thumbWritable = await thumbHandle.createWritable();
      await thumbWritable.write(thumbBlob);
      await thumbWritable.close();
      item.thumb = URL.createObjectURL(thumbBlob);
    }
  };

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
        ) : isEdit ? (
          <div className="relative flex">
            <ReactCrop
              crop={crop}
              onChange={(_, p) => setCrop(p)}
              className="max-h-[90vh] max-w-[90vw]"
            >
              <img src={item.url()} alt="media" />
            </ReactCrop>
            <div className="absolute bottom-0 mt-2 flex w-full justify-end gap-2 p-2">
              <Button
                onClick={async () => {
                  await cropAndWrite();
                  setIsEdit(false);
                }}
                size="sm"
                className={cn(
                  "text-foreground rounded-3xl bg-black/70 backdrop-blur-xl hover:bg-black/80 active:bg-black/90",
                  !crop || crop.width <= 0 || crop.height <= 0
                    ? "cursor-not-allowed opacity-50"
                    : "",
                )}
                disabled={!crop || crop.width <= 0 || crop.height <= 0}
              >
                Save Crop
              </Button>
              <Button
                variant="outline"
                onClick={() => setIsEdit(false)}
                size="sm"
                className="text-foreground rounded-3xl bg-black/50 backdrop-blur-xl hover:bg-black/60 active:bg-black/70"
              >
                Cancel
              </Button>
            </div>
          </div>
        ) : (
          <img
            src={item.url()}
            className="max-h-[90vh] max-w-[90vw]"
            alt="media"
            onClick={() => setIsEdit(!isEdit)}
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
