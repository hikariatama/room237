"use client";

import { useCallback } from "react";
import { toast } from "sonner";
import exifr from "exifr";
import { isMedia } from "@/lib/utils";
import { MetadataManager } from "@/lib/metadata-manager";
import type { Album, MediaEntry } from "@/lib/types";

const thumbDir = async (a: Album) =>
  a.handle.getDirectoryHandle(".room237-thumb", { create: true });

const makeAvif = async (bmp: CanvasImageSource): Promise<Blob> => {
  const size = 400;
  const { width, height } = bmp as { width: number; height: number };
  const aspectRatio = width / height;

  let newWidth: number;
  let newHeight: number;

  if (width < height) {
    newWidth = size;
    newHeight = size / aspectRatio;
  } else {
    newHeight = size;
    newWidth = size * aspectRatio;
  }

  const c = new OffscreenCanvas(newWidth, newHeight);
  const ctx = c.getContext("2d")!;
  ctx.drawImage(bmp, 0, 0, newWidth, newHeight);
  return c.convertToBlob({ type: "image/avif", quality: 0.4 });
};

const makeVideoThumb = async (f: File): Promise<Blob> => {
  const v = document.createElement("video");
  v.src = URL.createObjectURL(f);
  await new Promise<void>((resolve, reject) => {
    v.onloadedmetadata = () => resolve();
    v.onerror = reject;
  });
  v.currentTime = Math.min(10, v.duration * Math.random());
  await new Promise((resolve, reject) => {
    v.onseeked = resolve;
    v.onerror = reject;
  });
  v.pause();
  const size = 400;
  const { videoWidth: width, videoHeight: height } = v;
  const aspectRatio = width / height;
  let newWidth: number;
  let newHeight: number;
  if (width < height) {
    newWidth = size;
    newHeight = size / aspectRatio;
  } else {
    newHeight = size;
    newWidth = size * aspectRatio;
  }
  const c = new OffscreenCanvas(newWidth, newHeight);
  const ctx = c.getContext("2d")!;
  ctx.drawImage(v, 0, 0, newWidth, newHeight);
  const blob = await c.convertToBlob({ type: "image/avif", quality: 0.4 });
  URL.revokeObjectURL(v.src);
  return blob;
};

export const imgThumb = async (f: File) => {
  if (f.type.startsWith("image")) {
    return await makeAvif(await createImageBitmap(f));
  }
  if (f.type.startsWith("video")) {
    return await makeVideoThumb(f);
  }
  throw new Error("Unsupported file type for thumbnail generation");
};

const videoThumb = async (f: File) => {
  const v = document.createElement("video");
  v.src = URL.createObjectURL(f);
  await v.play().catch(() => undefined);
  v.pause();
  const b = await makeAvif(v);
  URL.revokeObjectURL(v.src);
  return b;
};

export function useUpload(
  albums: Album[],
  refresh: () => Promise<void>,
  active: Album | null,
  onAdded: (e: MediaEntry) => void,
) {
  const addFilesToAlbum = useCallback(
    async (album: Album, files: FileList | File[]) => {
      const list = Array.from(files).filter((f) => isMedia(f.name));
      if (!list.length) return;
      let done = 0;
      const id = toast.loading(
        `Adding media to "${album.name}" (${done}/${list.length})`,
        { duration: Infinity },
      );
      const td = await thumbDir(album);

      for (const file of list) {
        if (album.images.includes(file.name)) {
          done++;
          toast.loading(
            `Adding media to "${album.name}" (${done}/${list.length})`,
            { id, duration: Infinity },
          );
          continue;
        }

        const fh = await album.handle.getFileHandle(file.name, {
          create: true,
        });
        const w = await fh.createWritable();
        await w.write(await file.arrayBuffer());
        await w.close();

        const thumbBlob = file.type.startsWith("image")
          ? await imgThumb(file)
          : await videoThumb(file);
        const thName = file.name.replace(/\.[^.]+$/, ".avif");
        const thHandle = await td.getFileHandle(thName, { create: true });
        const tw = await thHandle.createWritable();
        await tw.write(await thumbBlob.arrayBuffer());
        await tw.close();
        const thumbUrl = URL.createObjectURL(thumbBlob);

        const shoot = file.type.startsWith("image")
          ? await exifr
              .parse(file, ["DateTimeOriginal"])
              .then(
                (d: { DateTimeOriginal?: { getTime?: () => number } }) =>
                  d?.DateTimeOriginal?.getTime?.() ?? file.lastModified,
              )
              .catch(() => file.lastModified)
          : file.lastModified;

        const url = URL.createObjectURL(file);

        const entry: MediaEntry = {
          file,
          url: () => url,
          unload: () => URL.revokeObjectURL(url),
          thumb: thumbUrl,
          meta: { added: Date.now(), shoot },
          handle: fh,
        };

        await MetadataManager.writeFileMeta(
          album.handle,
          file.name,
          entry.meta,
        ).catch(() => undefined);
        album.images.push(file.name);
        if (active?.dirName === album.dirName) onAdded(entry);

        done++;
        toast.loading(
          `Adding media to "${album.name}" (${done}/${list.length})`,
          { id, duration: Infinity },
        );
      }
      await refresh();
      toast.success(`Added ${list.length} file(s)`, { id, duration: 2000 });
    },
    [active, onAdded, refresh],
  );

  const uploadFilesToActive = useCallback(
    async (f: FileList | File[]) => {
      if (!active) return;
      await addFilesToAlbum(active, f);
    },
    [active, addFilesToAlbum],
  );

  return { addFilesToAlbum, uploadFilesToActive };
}
