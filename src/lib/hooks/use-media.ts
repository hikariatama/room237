"use client";

import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import type { Album, LayoutType, MediaEntry } from "@/lib/types";
import { loadImageDims } from "@/lib/utils";
import * as exifr from "exifr";
import { imgThumb } from "./use-upload";

export type SortKey = "shoot" | "added" | "name";
export type SortDir = "asc" | "desc";

const cmp = (a: MediaEntry, b: MediaEntry, key: SortKey): number => {
  if (key === "shoot") {
    const d = (a.meta.shoot ?? 0) - (b.meta.shoot ?? 0);
    if (d) return d;
  }
  if (key === "added") {
    const d = (a.meta.added ?? 0) - (b.meta.added ?? 0);
    if (d) return d;
  }
  return a.file.name.localeCompare(b.file.name);
};

export function useMedia(
  album: Album | null,
  batch: number,
  sortKey: SortKey,
  sortDir: SortDir,
) {
  const [all, setAll] = useState<MediaEntry[]>([]);
  const [visibleCount, setVisibleCount] = useState<number>(30);
  const [layout, setLayoutInternal] = useState<LayoutType>("default");
  const urlCache = useRef(new Map<string, string>());

  const setLayout = useCallback((l: LayoutType) => {
    setLayoutInternal(l);
    localStorage.setItem("room237-layout", l);
  }, []);

  useEffect(() => {
    const storedLayout = localStorage.getItem("room237-layout");
    if (storedLayout) {
      setLayoutInternal(storedLayout as LayoutType);
    }
  }, []);

  const exifDate = async (f: File): Promise<number | null> => {
    try {
      const d = (await exifr.parse(f, ["DateTimeOriginal"])) as {
        DateTimeOriginal?: Date;
      };
      return d?.DateTimeOriginal?.getTime() ?? null;
    } catch {
      return null;
    }
  };

  const loadInitial = useCallback(async () => {
    if (!album) {
      setAll([]);
      setVisibleCount(30);
      return;
    }
    const entries: MediaEntry[] = [];
    for (const n of album.images) {
      const fh = await album.handle.getFileHandle(n);
      const file = await fh.getFile();

      let meta = await album.handle
        .getFileHandle(`.${n.replace(/\.[^.]+$/, "")}.json`)
        .then(
          async (h) =>
            JSON.parse(await (await h.getFile()).text()) as MediaEntry["meta"],
        )
        .catch(() => null);

      if (!meta) {
        meta = { name: n, added: file.lastModified };
        await fh
          .createWritable()
          .then((w) => w.write(JSON.stringify(meta)))
          .catch(() => undefined);
      }

      meta.added ??= file.lastModified;
      if (!meta.shoot && file.type.startsWith("image"))
        meta.shoot = (await exifDate(file)) ?? meta.added;
      if ((!meta.width || !meta.height) && file.type.startsWith("image")) {
        const { w, h } = await loadImageDims(file);
        meta = { ...meta, width: w, height: h };
      }

      const thumbDir = await album.handle.getDirectoryHandle(".room237-thumb", {
        create: true,
      });
      let thumbBlob: Blob | null = null;
      try {
        const thumbHandle = await thumbDir.getFileHandle(
          `${n.replace(/\.[^.]+$/, "")}.avif`,
        );
        const thumbFile = await thumbHandle.getFile();
        if (thumbFile.lastModified < Date.now() - 24 * 60 * 60 * 1000) {
          throw new Error("Thumbnail is too old");
        }
        thumbBlob = thumbFile.slice(0, thumbFile.size, "image/avif");
      } catch {}
      try {
        if (!thumbBlob) {
          thumbBlob = await imgThumb(file);
          const thumbHandle = await thumbDir.getFileHandle(
            `${n.replace(/\.[^.]+$/, "")}.avif`,
            { create: true },
          );
          const thumbWritable = await thumbHandle.createWritable();
          await thumbWritable.write(thumbBlob);
          await thumbWritable.close();
        }
      } catch {}

      if (!thumbBlob) {
        console.warn(`No thumbnail for ${n}`);
        continue;
      }

      const thumbUrl = URL.createObjectURL(thumbBlob);

      entries.push({
        file,
        meta,
        handle: fh,
        thumb: thumbUrl,
        url: () => {
          let url = urlCache.current.get(n);
          if (!url) {
            url = URL.createObjectURL(file);
            urlCache.current.set(n, url);
          }
          return url;
        },
        unload: () => {
          const url = urlCache.current.get(n);
          if (url) {
            URL.revokeObjectURL(url);
            urlCache.current.delete(n);
          }
        },
      });
    }
    setAll(entries);
    setVisibleCount(Math.min(entries.length, batch));
  }, [album, batch]);

  useEffect(() => {
    void loadInitial();
  }, [loadInitial]);

  const loadMore = () =>
    setVisibleCount((p) => {
      const newCount = p + batch;
      if (newCount >= all.length) return all.length;
      return newCount;
    });

  const sorted = useMemo(() => {
    const arr = [...all];
    arr.sort((a, b) => cmp(a, b, sortKey));
    if (sortDir === "desc") arr.reverse();
    return arr.slice(0, visibleCount);
  }, [visibleCount, all, sortKey, sortDir]);

  const addEntry = (e: MediaEntry) =>
    setAll((p) => (p.some((i) => i.file.name === e.file.name) ? p : [e, ...p]));
  const removeEntry = (e: MediaEntry) => {
    urlCache.current.delete(e.file.name);
    setAll((p) => p.filter((i) => i.file.name !== e.file.name));
  };

  return { media: sorted, loadMore, addEntry, removeEntry, layout, setLayout };
}
