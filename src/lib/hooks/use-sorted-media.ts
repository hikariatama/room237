"use client";

import { useMemo } from "react";
import { useRoom237 } from "../stores";
import { useActiveAlbum } from "./use-active-album";
import type { MediaEntry } from "../types";
import { FAVORITES_ALBUM_ID } from "../consts";
import { useStoreWithEqualityFn } from "zustand/traditional";
import { isEqual } from "lodash";

export type SortedMediaEntry = MediaEntry & { index: number };

export type SortedMediaResult = {
  media: Record<string, SortedMediaEntry>;
  mediaArray: SortedMediaEntry[];
  mediaPaths: string[];
};

export function useSortedMedia(): SortedMediaResult {
  const activeAlbum = useActiveAlbum();
  const sortKey = useRoom237((state) => state.sortKey);
  const sortDir = useRoom237((state) => state.sortDir);
  const favoritesOnly = useRoom237((state) => state.favoritesOnly);
  const randomSeed = useRoom237((state) => state.randomSeed);
  const activeAlbumId = useRoom237((state) => state.activeAlbumId);
  const { medias } = useStoreWithEqualityFn(
    useRoom237,
    (state): { medias: MediaEntry[] | null; path: string | null } => {
      if (!activeAlbumId) return { medias: null, path: null };
      if (activeAlbumId === FAVORITES_ALBUM_ID) {
        return {
          medias: state.albumMediasByPath[FAVORITES_ALBUM_ID] ?? null,
          path: FAVORITES_ALBUM_ID,
        };
      }
      const album = state.albumsById[activeAlbumId];
      if (!album) return { medias: null, path: null };
      return {
        medias: state.albumMediasByPath[album.path] ?? null,
        path: album.path,
      };
    },
    (a, b) => {
      if (a.path !== b.path) return false;
      if (a.medias === b.medias) return true;
      if (!a.medias || !b.medias) return false;
      if (a.medias.length !== b.medias.length) return false;
      for (let i = 0; i < a.medias.length; i++) {
        if (a.medias[i]?.path !== b.medias[i]?.path) return false;
        if (!isEqual(a.medias[i]?.meta, b.medias[i]?.meta)) return false;
      }
      return true;
    },
  );

  const sortedMedia = useMemo(() => {
    if (!activeAlbum || !medias) {
      return {
        media: {} as Record<string, SortedMediaEntry>,
        mediaArray: [] as SortedMediaEntry[],
        mediaPaths: [] as string[],
      };
    }

    const sorted = activeAlbum.getSortedMediaMap(
      medias,
      sortKey,
      sortDir,
      favoritesOnly,
      randomSeed,
    );

    return {
      media: sorted,
      mediaArray: Object.values(sorted),
      mediaPaths: Object.keys(sorted),
    };
  }, [activeAlbum, medias, sortKey, sortDir, favoritesOnly, randomSeed]);

  return sortedMedia;
}
