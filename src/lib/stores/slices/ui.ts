import { FAVORITES_ALBUM_ID } from "@/lib/consts";
import {
  moveMedia,
  setMediaFavorite,
  setMediaTimestamp,
} from "@/lib/fs/albumService";
import type { DetachedMediaEntry, MediaEntry } from "@/lib/types";
import type { Album } from "@/lib/types/album";
import { attachMediaEntry, isMedia } from "@/lib/utils";
import { join, tempDir } from "@tauri-apps/api/path";
import { remove, writeFile } from "@tauri-apps/plugin-fs";
import { invoke } from "@tauri-apps/api/core";
import { translate } from "@/lib/i18n";
import type { CustomStateCreator, UISlice } from "../types";
import { toast } from "@/components/toaster";

type FileWithPath = File & { path?: string };

const hasFilePath = (file: File): file is FileWithPath =>
  "path" in file && typeof (file as FileWithPath).path === "string";

const guessExtFromType = (type: string): string => {
  if (!type) return "bin";
  if (type === "image/jpeg") return "jpg";
  if (type === "image/png") return "png";
  if (type === "image/webp") return "webp";
  if (type === "image/gif") return "gif";
  if (type === "image/avif") return "avif";
  if (type === "image/bmp") return "bmp";
  if (type === "image/svg+xml") return "svg";
  if (type === "video/mp4") return "mp4";
  if (type === "video/webm") return "webm";
  if (type === "video/ogg") return "ogg";
  return "bin";
};

const proposedFileName = (file: File, idx: number) => {
  const trimmed = file.name?.trim();
  if (trimmed) return trimmed;
  const ext = guessExtFromType(file.type);
  return `pasted-${Date.now()}-${idx}.${ext}`;
};

export const uiSlice: CustomStateCreator<UISlice> = (set, get) => ({
  columns: 4,
  sortKey: "shoot",
  sortDir: "desc",
  language: "en",
  selection: [],
  draggedItems: [],
  dragHoverHint: null,
  layout: "default",
  showDuplicates: false,
  favoritesOnly: false,
  randomSeed: 1,
  fileManagerName: null,
  viewerIndex: null,
  batchOperationInProgress: false,
  setBatchOperationInProgress: (inProgress) =>
    set({ batchOperationInProgress: inProgress }),
  setFileManagerName: (fileManagerName) => set({ fileManagerName }),
  setDraggedItems: (items) => set({ draggedItems: items }),
  clearDraggedItems: () => set({ draggedItems: [] }),
  setDragHoverHint: (hint) => set({ dragHoverHint: hint }),
  clearDragHoverHint: () => set({ dragHoverHint: null }),
  setColumns: (n) => set({ columns: n }),
  setSortKey: (k) => set({ sortKey: k }),
  setSortDir: (d) => set({ sortDir: d }),
  setLanguage: (language) => set({ language }),
  setLayout: (l) => set({ layout: l }),
  toggleSelection: (media, additive) => {
    if (!additive) return;
    const n = get().selection.slice();
    const idx = n.findIndex((m) => m.path === media.path);
    if (idx !== -1) n.splice(idx, 1);
    else n.push(media);
    set({ selection: n });
  },
  selectRange: (media) => {
    const state = get();
    const selection = state.selection;

    if (selection.length === 0) {
      set({ selection: [media] });
      return;
    }

    const lastSelected = selection[selection.length - 1];
    if (!lastSelected) {
      set({ selection: [media] });
      return;
    }

    const activeAlbumId = state.activeAlbumId;
    if (!activeAlbumId) return;

    const activeAlbum =
      activeAlbumId === FAVORITES_ALBUM_ID
        ? state.favoritesAlbum
        : state.albumsById[activeAlbumId];
    if (!activeAlbum) return;

    const medias =
      activeAlbumId === FAVORITES_ALBUM_ID
        ? state.albumMediasByPath[FAVORITES_ALBUM_ID]
        : state.albumMediasByPath[activeAlbum.path];
    if (!medias) return;

    const sortedMedia = activeAlbum.getSortedMedia(
      medias,
      state.sortKey,
      state.sortDir,
      state.favoritesOnly,
      state.randomSeed,
    );

    const lastIndex = sortedMedia.findIndex(
      (m) => m.path === lastSelected.path,
    );
    const currentIndex = sortedMedia.findIndex((m) => m.path === media.path);

    if (lastIndex === -1 || currentIndex === -1) return;

    const startIndex = Math.min(lastIndex, currentIndex);
    const endIndex = Math.max(lastIndex, currentIndex);

    const rangeItems = sortedMedia.slice(startIndex, endIndex + 1);

    const existingPaths = new Set(selection.map((m) => m.path));
    const newItems = rangeItems.filter((m) => !existingPaths.has(m.path));

    set({ selection: [...selection, ...newItems] });
  },
  clearSelection: () => set({ selection: [] }),
  selectAll: () => {
    const state = get();
    const activeAlbumId = state.activeAlbumId;
    if (!activeAlbumId) return;
    const activeAlbum =
      activeAlbumId === FAVORITES_ALBUM_ID
        ? state.favoritesAlbum
        : state.albumsById[activeAlbumId];
    if (!activeAlbum) return;
    const media =
      activeAlbumId === FAVORITES_ALBUM_ID
        ? state.albumMediasByPath[FAVORITES_ALBUM_ID]
        : state.albumMediasByPath[activeAlbum.path];
    if (!media) return;
    set({ selection: media });
  },
  setShowDuplicates: (show) => set({ showDuplicates: show }),
  setFavoritesOnly: (favoritesOnly) => set({ favoritesOnly }),
  setRandomSeed: (randomSeed) => set({ randomSeed }),
  openViewer: (index: number) => set({ viewerIndex: index }),
  closeViewer: () => set({ viewerIndex: null }),
  nextViewer: () => {
    const state = get();
    const activeAlbumId = state.activeAlbumId;
    if (!activeAlbumId) return;

    const activeAlbum: Album | null | undefined =
      activeAlbumId === FAVORITES_ALBUM_ID
        ? (state.favoritesAlbum ?? undefined)
        : state.albumsById[activeAlbumId];
    const medias =
      activeAlbumId === FAVORITES_ALBUM_ID
        ? state.albumMediasByPath[FAVORITES_ALBUM_ID]
        : activeAlbum
          ? state.albumMediasByPath[activeAlbum.path]
          : null;
    if (!activeAlbum || !medias) return;

    const sortedMedia = activeAlbum.getSortedMedia(
      medias,
      state.sortKey,
      state.sortDir,
      state.favoritesOnly,
      state.randomSeed,
    );
    if (state.viewerIndex === null || !sortedMedia.length) return;
    const currentIndex = sortedMedia.findIndex(
      (m) => m.index === state.viewerIndex,
    );
    const nextIndex =
      currentIndex >= 0 ? (currentIndex + 1) % sortedMedia.length : 0;
    set({ viewerIndex: sortedMedia[nextIndex]?.index ?? null });
  },
  prevViewer: () => {
    const state = get();
    const activeAlbumId = state.activeAlbumId;
    if (!activeAlbumId) return;

    const activeAlbum: Album | null | undefined =
      activeAlbumId === FAVORITES_ALBUM_ID
        ? (state.favoritesAlbum ?? undefined)
        : state.albumsById[activeAlbumId];
    const medias =
      activeAlbumId === FAVORITES_ALBUM_ID
        ? state.albumMediasByPath[FAVORITES_ALBUM_ID]
        : activeAlbum
          ? state.albumMediasByPath[activeAlbum.path]
          : null;
    if (!activeAlbum || !medias) return;

    const sortedMedia = activeAlbum.getSortedMedia(
      medias,
      state.sortKey,
      state.sortDir,
      state.favoritesOnly,
      state.randomSeed,
    );
    if (state.viewerIndex === null || !sortedMedia.length) return;
    const currentIndex = sortedMedia.findIndex(
      (m) => m.index === state.viewerIndex,
    );
    const prevIndex =
      currentIndex > 0 ? currentIndex - 1 : sortedMedia.length - 1;
    set({ viewerIndex: sortedMedia[prevIndex]?.index ?? null });
  },
  toggleFavorite: async (media: MediaEntry) => {
    await get().setFavorite(media, !media.favorite);
  },
  setFavorite: async (media: MediaEntry, favorite: boolean) => {
    await setMediaFavorite(media.path, favorite);
    const state = get();
    const activeAlbumId = state.activeAlbumId;
    if (activeAlbumId) {
      const activeAlbum: Album | null | undefined =
        activeAlbumId === FAVORITES_ALBUM_ID
          ? (state.favoritesAlbum ?? undefined)
          : state.albumsById[activeAlbumId];
      const albumMedias =
        activeAlbumId === FAVORITES_ALBUM_ID
          ? (state.albumMediasByPath[FAVORITES_ALBUM_ID] ?? null)
          : activeAlbum
            ? (state.albumMediasByPath[activeAlbum.path] ?? null)
            : null;
      if (activeAlbum && albumMedias) {
        const mediaIndex = albumMedias.findIndex((m) => m.path === media.path);
        if (mediaIndex !== -1) {
          const updated = [
            ...albumMedias.slice(0, mediaIndex),
            { ...albumMedias[mediaIndex]!, favorite },
            ...albumMedias.slice(mediaIndex + 1),
          ];
          activeAlbum.invalidateCache();
          set((current) => ({
            albumMediasByPath: {
              ...current.albumMediasByPath,
              [activeAlbumId === FAVORITES_ALBUM_ID
                ? FAVORITES_ALBUM_ID
                : activeAlbum.path]: updated,
            },
          }));
        }
      }
    }
    await get().refreshFavoritesMap();
  },
  deleteMedias: async (medias: MediaEntry[]) => {
    const state = get();
    if (!medias.length) return;

    const loadingToast = toast.loading(
      translate(get().language, "toast.deleteMedia.loading", {
        count: medias.length,
      }),
    );

    set({ batchOperationInProgress: true });
    try {
      const grouped = medias.reduce<Record<string, MediaEntry[]>>(
        (acc, media) => {
          const key = media.albumId;
          acc[key] = acc[key] ? [...acc[key], media] : [media];
          return acc;
        },
        {},
      );

      for (const [albumId, items] of Object.entries(grouped)) {
        const album = state.albumsById[albumId];
        if (!album || album.path === FAVORITES_ALBUM_ID) continue;
        for (const media of items) {
          await remove(media.path);
        }
        await get().loadAlbumMedia(album.albumId, { force: true });
      }

      await get().refreshFavoritesMap();
      set({ selection: [], viewerIndex: null });
      loadingToast.success(
        translate(get().language, "toast.deleteMedia.success", {
          count: medias.length,
        }),
      );
    } catch (e) {
      console.error("Failed to delete", e);
      loadingToast.error(
        translate(get().language, "toast.deleteMedia.error", {
          count: medias.length,
        }),
      );
    } finally {
      set({ batchOperationInProgress: false });
    }
  },
  deleteMedia: async (media: MediaEntry) => {
    await get().deleteMedias([media]);
  },
  moveMediasToAlbum: async (album: Album, medias: MediaEntry[]) => {
    const state = get();
    if (!medias.length || album.path === FAVORITES_ALBUM_ID) return;

    set({ batchOperationInProgress: true });
    try {
      const albums = state.albumsById;
      const grouped = medias.reduce<Record<string, MediaEntry[]>>(
        (acc, media) => {
          if (media.albumId === album.albumId) return acc;
          acc[media.albumId] = acc[media.albumId]
            ? [...acc[media.albumId]!, media]
            : [media];
          return acc;
        },
        {},
      );
      const touched = new Set<string>();

      for (const [albumId, items] of Object.entries(grouped)) {
        const source = albums[albumId];
        if (!source || source.path === FAVORITES_ALBUM_ID) continue;
        await moveMedia(source, album, items, {
          language: get().language,
        });
        touched.add(source.albumId);
      }

      if (touched.size === 0) return;
      touched.add(album.albumId);

      for (const id of touched) {
        if (!albums[id]) continue;
        await get().loadAlbumMedia(id, { force: true });
      }

      await get().refreshFavoritesMap();
      set({ selection: [], viewerIndex: null });
    } finally {
      set({ batchOperationInProgress: false });
    }
  },
  patchMediaDates: (medias: MediaEntry[], timestamp: number) => {
    const state = get();
    if (!medias.length) return () => undefined;
    const albums = state.albumsById;
    const mediaPaths = new Set(medias.map((m) => m.path));
    const previousByAlbum = new Map<string, MediaEntry[]>();
    const updatedMedias: Record<string, MediaEntry[]> = {};
    const updateAlbumMedias = (album: Album | null | undefined) => {
      if (!album) return false;
      const current = get().albumMediasByPath[album.path];
      if (!current) return false;
      if (!previousByAlbum.has(album.albumId)) {
        previousByAlbum.set(album.albumId, current);
      }
      let changed = false;
      const nextMedias = current.map((media) => {
        if (!mediaPaths.has(media.path)) return media;
        changed = true;
        return {
          ...media,
          meta: {
            ...media.meta,
            shoot: timestamp,
            added: media.meta.added ?? timestamp,
          },
        };
      });
      if (!changed) return false;
      updatedMedias[album.path] = nextMedias;
      album.invalidateCache();
      return true;
    };

    medias.forEach((media) => updateAlbumMedias(albums[media.albumId] ?? null));

    const favoritesAlbum = state.favoritesAlbum;
    const favoritesMedias = favoritesAlbum
      ? (get().albumMediasByPath[FAVORITES_ALBUM_ID] ?? null)
      : null;
    let favoritesPrev: MediaEntry[] | null = null;
    let favoritesChanged = false;
    if (favoritesAlbum && favoritesMedias) {
      favoritesPrev = favoritesMedias;
      const nextFavorites = favoritesMedias.map((media) => {
        if (!mediaPaths.has(media.path)) return media;
        favoritesChanged = true;
        return {
          ...media,
          meta: {
            ...media.meta,
            shoot: timestamp,
            added: media.meta.added ?? timestamp,
          },
        };
      });
      if (favoritesChanged) {
        favoritesAlbum.invalidateCache();
        updatedMedias[FAVORITES_ALBUM_ID] = nextFavorites;
      }
    }

    if (Object.keys(updatedMedias).length > 0) {
      set((current) => ({
        albumMediasByPath: {
          ...current.albumMediasByPath,
          ...updatedMedias,
        },
      }));
    }

    return () => {
      const rollbackMedias: Record<string, MediaEntry[]> = {};
      previousByAlbum.forEach((prev, albumId) => {
        const album = albums[albumId];
        if (!album) return;
        rollbackMedias[album.path] = prev;
        album.invalidateCache();
      });
      if (favoritesChanged && favoritesPrev && favoritesAlbum) {
        rollbackMedias[FAVORITES_ALBUM_ID] = favoritesPrev;
        favoritesAlbum.invalidateCache();
      }
      if (Object.keys(rollbackMedias).length > 0) {
        set((current) => ({
          albumMediasByPath: {
            ...current.albumMediasByPath,
            ...rollbackMedias,
          },
        }));
      }
    };
  },
  updateMediaDates: async (medias: MediaEntry[], timestamp: number) => {
    if (!medias.length) return;
    const rollback = get().patchMediaDates(medias, timestamp);
    const state = get();
    const albums = state.albumsById;
    const grouped = medias.reduce<Record<string, MediaEntry[]>>(
      (acc, media) => {
        acc[media.albumId] = acc[media.albumId]
          ? [...acc[media.albumId]!, media]
          : [media];
        return acc;
      },
      {},
    );

    try {
      const updatesByAlbum: Record<string, DetachedMediaEntry[]> = {};
      for (const [albumId, items] of Object.entries(grouped)) {
        const album = albums[albumId];
        if (!album) continue;
        const result = await setMediaTimestamp(
          album.path,
          items.map((m) => m.name),
          Math.floor(timestamp),
        );
        updatesByAlbum[albumId] = result;
      }

      const updatesForState: Record<string, MediaEntry[]> = {};
      Object.entries(updatesByAlbum).forEach(([albumId, entries]) => {
        const album = albums[albumId];
        if (!album || !entries.length) return;
        const current = get().albumMediasByPath[album.path];
        if (!current) return;
        const byName = new Map(entries.map((entry) => [entry.name, entry]));
        const nextMedias = current.map((media) => {
          const entry = byName.get(media.name);
          if (!entry) return media;
          const favorite = entry.favorite ?? media.favorite ?? false;
          const attached = attachMediaEntry(
            album.path,
            { ...entry, favorite },
            album.name,
            album.albumId,
          );
          return { ...attached, favorite };
        });
        album.invalidateCache();
        updatesForState[album.path] = nextMedias;
      });

      if (Object.keys(updatesForState).length > 0) {
        set((state) => ({
          albumMediasByPath: {
            ...state.albumMediasByPath,
            ...updatesForState,
          },
        }));
      }

      await get().refreshFavoritesMap();
      set({ selection: [], viewerIndex: null });
    } catch (error) {
      rollback();
      const message =
        error instanceof Error && error.message
          ? error.message
          : translate(get().language, "toast.failedDate");
      toast.error(message);
      throw error;
    }
  },
  addFilesToAlbum: async (album: Album, files: FileList | File[]) => {
    const candidates = Array.from(files).map((file, idx) => ({
      file,
      name: proposedFileName(file, idx),
    }));
    const mediaFiles = candidates.filter(({ name }) => isMedia(name));
    if (!mediaFiles.length) return;
    const lang = get().language;

    const loadingToast = toast.loading(
      translate(lang, "toast.addingToAlbum", {
        count: mediaFiles.length,
        values: { count: mediaFiles.length },
        defaultValue: `Uploading ${mediaFiles.length} file(s)...`,
      }),
    );

    const tempFiles: string[] = [];

    set({ batchOperationInProgress: true });
    try {
      const pasteTempDir = await tempDir();
      const now = Date.now();
      const payload = await Promise.all(
        mediaFiles.map(async ({ file, name }, idx) => {
          const sourcePath = hasFilePath(file) ? file.path : undefined;
          if (!sourcePath) {
            const buffer = await file.arrayBuffer();
            const bytes = new Uint8Array(buffer);
            const tmpName = `room237-paste-${now}-${idx}-${name}`;
            const tmpPath = await join(pasteTempDir, tmpName);
            await writeFile(tmpPath, bytes);
            tempFiles.push(tmpPath);
            return { name, sourcePath: tmpPath };
          }
          return { name, sourcePath };
        }),
      );

      const added = await invoke<DetachedMediaEntry[]>("add_media_files", {
        dir: album.path,
        files: payload,
      });

      if (added.length > 0) {
        const attached = added.map((entry) =>
          attachMediaEntry(album.path, entry, album.name, album.albumId),
        );
        const existing = get().albumMediasByPath[album.path] ?? null;
        if (existing) {
          const nextMedias = [...existing, ...attached];
          album.size = nextMedias.length;
          album.invalidateCache();
          set((state) => ({
            albumMediasByPath: {
              ...state.albumMediasByPath,
              [album.path]: nextMedias,
            },
          }));
          get().triggerAlbumUpdate(album.albumId);
        } else {
          await get().loadAlbumMedia(album.albumId, { force: true });
        }
        loadingToast.success(
          translate(lang, "toast.addedFiles", {
            count: added.length,
            values: { count: added.length },
            defaultValue: `Added ${added.length} file(s)`,
          }),
        );
      } else {
        loadingToast.error(translate(lang, "toast.noFilesAdded"));
      }
    } catch (error) {
      console.error("Failed to add files", error);
      const message =
        error instanceof Error && error.message
          ? error.message
          : translate(lang, "toast.failedAdd");
      loadingToast.error(message);
    } finally {
      for (const path of tempFiles) {
        try {
          await remove(path);
        } catch (err) {
          console.warn("Failed to cleanup temp paste file", err);
        }
      }
      set({ batchOperationInProgress: false });
    }
  },
  uploadFilesToActive: async (files: FileList | File[]) => {
    const state = get();
    const activeAlbumId = state.activeAlbumId;
    const activeAlbum =
      activeAlbumId && activeAlbumId !== FAVORITES_ALBUM_ID
        ? state.albumsById[activeAlbumId]
        : null;
    if (!activeAlbum) return;
    await get().addFilesToAlbum(activeAlbum, files);
  },
  moveDraggedToAlbum: async (album: Album) => {
    const medias = get().draggedItems;
    set({ draggedItems: [] });
    if (!medias.length) return;
    await get().moveMediasToAlbum(album, medias);
  },
  moveSelectedToAlbum: async (albumId: string) => {
    const state = get();
    const albums = state.albumsById;
    if (!albums[albumId]) return;
    const medias = Array.from(state.selection);
    if (!medias.length) return;
    await get().moveMediasToAlbum(albums[albumId], medias);
  },
});
