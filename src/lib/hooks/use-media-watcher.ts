"use client";

import { useEffect } from "react";
import { exists, watchImmediate } from "@tauri-apps/plugin-fs";
import path from "path";
import { FAVORITES_ALBUM_ID } from "../consts";
import { registerNewMedia } from "../fs/albumService";
import { useRoom237 } from "../stores";
import { isMedia } from "../utils";
import { useStoreWithEqualityFn } from "zustand/traditional";

export function useMediaWatcher() {
  const activeAlbum = useStoreWithEqualityFn(
    useRoom237,
    (state) => {
      if (!state.activeAlbumId) return null;
      const album = state.albumsById[state.activeAlbumId];
      if (!album) return null;
      return {
        id: album.albumId,
        path: album.path,
      };
    },
    (a, b) => a?.id === b?.id && a?.path === b?.path,
  );
  const triggerAlbumUpdate = useRoom237((state) => state.triggerAlbumUpdate);
  const urlCache = useRoom237((state) => state.urlCache);
  const loadAlbumMedia = useRoom237((state) => state.loadAlbumMedia);
  const isAlbumLoaded = useRoom237((state) => state.isAlbumLoaded);
  const batchOperationInProgress = useRoom237(
    (state) => state.batchOperationInProgress,
  );

  useEffect(() => {
    if (!activeAlbum || activeAlbum.path === FAVORITES_ALBUM_ID) return;

    let unwatch: () => void = () => null;

    void (async () => {
      unwatch = await watchImmediate(activeAlbum.path, (event) => {
        void (async () => {
          if (typeof event.type === "string") return;

          const state = useRoom237.getState();
          if (state.batchOperationInProgress) return;

          const album = useRoom237.getState().albumsById[activeAlbum.id];
          if (!album) return;
          if ("create" in event.type) {
            const entry = event.type.create;
            if (entry.kind !== "file") return;
            for (const mediaPath of event.paths) {
              if (
                path.normalize(mediaPath) !==
                path.normalize(
                  path.join(activeAlbum.path, path.basename(mediaPath)),
                )
              )
                return;
              if (isMedia(mediaPath)) {
                await registerNewMedia(album, path.basename(mediaPath));
                if (isAlbumLoaded(activeAlbum.id)) {
                  await loadAlbumMedia(activeAlbum.id, { force: true });
                  triggerAlbumUpdate(activeAlbum.id);
                }
              }
            }
          } else if ("modify" in event.type) {
            const entry = event.type.modify;
            if (entry.kind !== "rename") return;
            for (const mediaPath of event.paths) {
              if (
                path.normalize(mediaPath) !==
                path.normalize(
                  path.join(activeAlbum.path, path.basename(mediaPath)),
                )
              )
                return;
              const filename = path.basename(mediaPath);
              if (!(await exists(mediaPath))) {
                if (urlCache.has(filename)) {
                  urlCache.delete(filename);
                }
              } else {
                await registerNewMedia(album, path.basename(mediaPath));
              }
              if (isAlbumLoaded(activeAlbum.id)) {
                await loadAlbumMedia(activeAlbum.id, { force: true });
                triggerAlbumUpdate(activeAlbum.id);
              }
            }
          } else if ("remove" in event.type) {
            const entry = event.type.remove;
            if (entry.kind !== "file") return;
            for (const mediaPath of event.paths) {
              if (
                path.normalize(mediaPath) !==
                path.normalize(
                  path.join(activeAlbum.path, path.basename(mediaPath)),
                )
              )
                return;
              const filename = path.basename(mediaPath);
              if (urlCache.has(filename)) {
                urlCache.delete(filename);
              }
              if (isAlbumLoaded(activeAlbum.id)) {
                await loadAlbumMedia(activeAlbum.id, { force: true });
                triggerAlbumUpdate(activeAlbum.id);
              }
            }
          }
        })();
      });
    })();

    return () => {
      unwatch();
    };
  }, [
    activeAlbum,
    triggerAlbumUpdate,
    urlCache,
    isAlbumLoaded,
    loadAlbumMedia,
    batchOperationInProgress,
  ]);
}
