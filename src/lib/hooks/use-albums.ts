"use client";

import { useCallback, useEffect, useState } from "react";
import type { Album } from "@/lib/types";
import {
  listAlbums,
  createAlbum as _create,
  deleteAlbum as _delete,
} from "../fs/albumService";

export function useAlbums(rootDir: FileSystemDirectoryHandle | null) {
  const [albums, setAlbums] = useState<Album[]>([]);
  const [active, setActive] = useState<string>("");
  const refresh = useCallback(async () => {
    if (!rootDir) return;
    const a = await listAlbums(rootDir);
    setAlbums(a);
    if (!active && a.length) setActive(a[0]!.dirName);
  }, [rootDir, active]);
  useEffect(() => {
    void refresh();
  }, [refresh]);
  const createAlbum = async (name: string) => {
    if (!rootDir) return;
    const a = await _create(rootDir, name);
    setAlbums((p) => [...p, a]);
  };
  const deleteAlbum = async (album: Album) => {
    if (!rootDir) return;
    await _delete(rootDir, album);
    await refresh();
  };
  const activeAlbum = albums.find((a) => a.dirName === active) ?? null;
  return {
    albums,
    activeAlbum,
    setActive,
    createAlbum,
    deleteAlbum,
    refresh,
  };
}
