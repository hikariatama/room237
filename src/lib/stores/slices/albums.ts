import { FAVORITES_ALBUM_ID } from "@/lib/consts";
import {
  createAlbum as _createAlbum,
  deleteAlbum as _deleteAlbum,
  computeAggregatedSizes,
  listAlbums,
  moveAlbum as _moveAlbum,
  renameAlbum as _renameAlbum,
} from "@/lib/fs/albumService";
import { getStore } from "@/lib/fs/state";
import type { MediaEntry } from "@/lib/types";
import { fetchAlbumDuplicates, loadAlbumMedias } from "@/lib/types/album";
import type { Album, AlbumId, AlbumNode } from "@/lib/types/album";
import type { AlbumsSlice, CustomStateCreator } from "../types";
import { toast } from "@/components/toaster";

const applyAggregateSizes = (
  albumsById: Record<AlbumId, Album>,
): Record<AlbumId, Album> => {
  const totals = computeAggregatedSizes(albumsById);
  Object.entries(totals).forEach(([albumId, total]) => {
    if (albumsById[albumId]) {
      albumsById[albumId].totalSize = total;
    }
  });
  return albumsById;
};

const mediaLoadingByPath = new Map<string, Promise<MediaEntry[]>>();
const duplicatesLoadingByPath = new Map<string, Promise<string[][]>>();

export const albumsSlice: CustomStateCreator<AlbumsSlice> = (set, get) => ({
  rootDir: null,
  albumsReady: false,
  albumsById: {},
  albumMediasByPath: {},
  albumDuplicatesByPath: {},
  albumTree: [],
  expandedAlbumIds: new Set<AlbumId>(),
  manuallyExpandedAlbumIds: new Set<AlbumId>(),
  activeAlbumId: null,
  loadingAlbumId: null,
  duplicatesAvailable: false,
  duplicatesLoading: false,
  setDuplicatesAvailable: (duplicatesAvailable) => set({ duplicatesAvailable }),
  setDuplicatesLoading: (duplicatesLoading) => set({ duplicatesLoading }),
  setRootDir: (rootDir) =>
    set({
      rootDir,
      favoritesAlbum: null,
      favoritesMap: {},
      activeAlbumId: null,
      loadingAlbumId: null,
      expandedAlbumIds: new Set<AlbumId>(),
      manuallyExpandedAlbumIds: new Set<AlbumId>(),
      albumTree: [],
      albumsById: {},
      albumMediasByPath: {},
      albumDuplicatesByPath: {},
    }),
  setAlbumsReady: (albumsReady) => set({ albumsReady }),
  setAlbums: (albumsById, albumTree) =>
    set((state) => {
      const validPaths = new Set(
        Object.values(albumsById).map((album) => album.path),
      );
      validPaths.add(FAVORITES_ALBUM_ID);
      return {
        albumsById: applyAggregateSizes({ ...albumsById }),
        albumTree,
        albumMediasByPath: Object.fromEntries(
          Object.entries(state.albumMediasByPath).filter(([path]) =>
            validPaths.has(path),
          ),
        ),
        albumDuplicatesByPath: Object.fromEntries(
          Object.entries(state.albumDuplicatesByPath).filter(([path]) =>
            validPaths.has(path),
          ),
        ),
      };
    }),
  setAlbumTree: (albumTree: AlbumNode[]) => set({ albumTree }),
  setExpandedAlbumIds: (expandedAlbumIds) =>
    set({ expandedAlbumIds: new Set(expandedAlbumIds) }),
  setManuallyExpandedAlbumIds: (manuallyExpandedAlbumIds) =>
    set({ manuallyExpandedAlbumIds: new Set(manuallyExpandedAlbumIds) }),
  expandAlbum: (id) =>
    set((state) => {
      const expanded = new Set(state.expandedAlbumIds);
      expanded.add(id);
      return { expandedAlbumIds: expanded };
    }),
  collapseAlbum: (id) =>
    set((state) => {
      const expanded = new Set(state.expandedAlbumIds);
      expanded.delete(id);
      return { expandedAlbumIds: expanded };
    }),
  markManualExpand: (id) =>
    set((state) => {
      const manual = new Set(state.manuallyExpandedAlbumIds);
      manual.add(id);
      return { manuallyExpandedAlbumIds: manual };
    }),
  markManualCollapse: (id) =>
    set((state) => {
      const manual = new Set(state.manuallyExpandedAlbumIds);
      manual.delete(id);
      return { manuallyExpandedAlbumIds: manual };
    }),
  collapseAutoExpandedExcept: (keepId) =>
    set((state) => {
      const expanded = new Set(state.expandedAlbumIds);
      const keep = new Set<AlbumId>();
      if (keepId) {
        let current: AlbumId | null | undefined = keepId;
        while (current) {
          keep.add(current);
          current = state.albumsById[current]?.parentId ?? null;
        }
      }

      expanded.forEach((id) => {
        if (keep.has(id)) return;
        if (state.manuallyExpandedAlbumIds.has(id)) return;
        expanded.delete(id);
      });
      return { expandedAlbumIds: expanded };
    }),
  setActiveAlbumId: (activeAlbumId) => set({ activeAlbumId }),
  setLoadingAlbumId: (id) => set({ loadingAlbumId: id }),
  isAlbumLoaded: (albumId) => {
    const album = get().albumsById[albumId];
    if (!album) return false;
    const medias = get().albumMediasByPath[album.path];
    return Array.isArray(medias) && medias.length === album.size;
  },

  loadAlbumMedia: async (albumId, options) => {
    const album = get().albumsById[albumId];
    if (!album) return [];
    const path = album.path;
    const existing = get().albumMediasByPath[path];
    if (!options?.force && existing?.length === album.size) {
      return existing;
    }

    const inFlight = mediaLoadingByPath.get(path);
    if (inFlight) {
      return inFlight;
    }

    const task = (async () => {
      const medias = await loadAlbumMedias(album);
      set((state) => {
        const albumInState = state.albumsById[album.albumId];
        const cloned =
          albumInState &&
          (Object.assign(
            Object.create(Object.getPrototypeOf(albumInState) as object | null),
            albumInState,
            { size: medias.length },
          ) as Album);
        return {
          albumsById: cloned
            ? applyAggregateSizes({
                ...state.albumsById,
                [album.albumId]: cloned,
              })
            : state.albumsById,
          albumMediasByPath: {
            ...state.albumMediasByPath,
            [path]: medias,
          },
        };
      });
      return medias;
    })();

    mediaLoadingByPath.set(path, task);
    try {
      return await task;
    } finally {
      mediaLoadingByPath.delete(path);
    }
  },

  loadAlbumDuplicates: async (albumId, options) => {
    const album = get().albumsById[albumId];
    if (!album) return [];
    const path = album.path;
    const existing = get().albumDuplicatesByPath[path];
    if (!options?.force && existing) {
      return existing;
    }

    const inFlight = duplicatesLoadingByPath.get(path);
    if (inFlight) {
      return inFlight;
    }

    const task = (async () => {
      await get().loadAlbumMedia(albumId, options);
      const duplicates = await fetchAlbumDuplicates(album);
      set((state) => ({
        albumDuplicatesByPath: {
          ...state.albumDuplicatesByPath,
          [path]: duplicates,
        },
      }));
      return duplicates;
    })();

    duplicatesLoadingByPath.set(path, task);
    try {
      return await task;
    } finally {
      duplicatesLoadingByPath.delete(path);
    }
  },

  triggerAlbumUpdate: (albumId: AlbumId) => {
    const state = get();
    const album = state.albumsById[albumId];
    if (!album) return;
    const cloned = Object.assign(
      Object.create(Object.getPrototypeOf(album) as object | null),
      album,
    ) as Album;
    const nextAlbums = applyAggregateSizes({
      ...state.albumsById,
      [albumId]: cloned,
    });
    set({ albumsById: nextAlbums });
  },

  autoExpandAncestors: (albumId: AlbumId | null) => {
    if (!albumId) return;
    const state = get();
    const expanded = new Set(state.expandedAlbumIds);
    let current: AlbumId | undefined | null = albumId;
    while (current) {
      expanded.add(current);
      current = state.albumsById[current]?.parentId ?? null;
    }
    set({ expandedAlbumIds: expanded });
  },

  setActive: async (albumId: AlbumId) => {
    const state = get();
    const albums = state.albumsById;
    const favoritesAlbum = state.favoritesAlbum;

    set({ loadingAlbumId: albumId, activeAlbumId: albumId });

    if (albumId === FAVORITES_ALBUM_ID || albumId === "Favorites") {
      if (favoritesAlbum) {
        set({ favoritesOnly: false });
        get().triggerAlbumUpdate(favoritesAlbum.albumId);
        const store = await getStore();
        await store.set("activeAlbum", favoritesAlbum.albumId);
        await store.save();
        set({ loadingAlbumId: null });
        return;
      } else {
        set({ loadingAlbumId: null });
        return;
      }
    }

    const album = albums[albumId];
    if (!album) {
      set({ activeAlbumId: null, favoritesOnly: false });
      set({ loadingAlbumId: null });
      return;
    }

    get().autoExpandAncestors(albumId);

    const favoriteFilters = state.favoriteFilters;
    set({ favoritesOnly: favoriteFilters[album.albumId] ?? false });

    if (!get().isAlbumLoaded(albumId)) {
      await get().loadAlbumMedia(albumId);

      const currentState = get();
      if (currentState.activeAlbumId !== albumId) {
        return;
      }

      get().triggerAlbumUpdate(albumId);
      set({ loadingAlbumId: null });
    } else {
      get().triggerAlbumUpdate(albumId);
      set({ loadingAlbumId: null });
    }

    get().clearSelection();

    const store = await getStore();
    await store.set("activeAlbum", albumId);
    await store.save();
  },

  hardRefresh: async () => {
    const state = get();
    if (state.confirmOpenEnabled && !state.allowOpen) {
      return;
    }
    const rootDir =
      state.displayDecoy && state.decoyRoot ? state.decoyRoot : state.rootDir;
    if (!rootDir) return;
    const loadingToast = toast.loading("Loading albums...");
    const { albumsById, albumTree } = await listAlbums(rootDir);
    const validPaths = new Set(
      Object.values(albumsById).map((album) => album.path),
    );
    validPaths.add(FAVORITES_ALBUM_ID);
    set((state) => ({
      albumsById: applyAggregateSizes(albumsById),
      albumTree,
      expandedAlbumIds: new Set<AlbumId>(),
      albumMediasByPath: Object.fromEntries(
        Object.entries(state.albumMediasByPath).filter(([path]) =>
          validPaths.has(path),
        ),
      ),
      albumDuplicatesByPath: Object.fromEntries(
        Object.entries(state.albumDuplicatesByPath).filter(([path]) =>
          validPaths.has(path),
        ),
      ),
    }));
    loadingToast.dismiss();
    set({ albumsReady: true });
    const activeAlbumId = get().activeAlbumId;
    if (activeAlbumId) {
      get().autoExpandAncestors(activeAlbumId);
    }
    await get().refreshFavoritesMap();
    window.dispatchEvent(new Event("check-preload"));
  },

  hotRefresh: async () => {
    const state = get();
    if (state.confirmOpenEnabled && !state.allowOpen) {
      return;
    }
    const rootDir =
      state.displayDecoy && state.decoyRoot ? state.decoyRoot : state.rootDir;
    if (!rootDir) return;
    const { albumsById, albumTree } = await listAlbums(rootDir);
    const validIds = new Set(Object.keys(albumsById));
    const expanded = new Set(
      Array.from(get().expandedAlbumIds).filter((id) => validIds.has(id)),
    );
    const validPaths = new Set(
      Object.values(albumsById).map((album) => album.path),
    );
    validPaths.add(FAVORITES_ALBUM_ID);
    set((state) => ({
      albumsById: applyAggregateSizes(albumsById),
      albumTree,
      expandedAlbumIds: expanded,
      albumMediasByPath: Object.fromEntries(
        Object.entries(state.albumMediasByPath).filter(([path]) =>
          validPaths.has(path),
        ),
      ),
      albumDuplicatesByPath: Object.fromEntries(
        Object.entries(state.albumDuplicatesByPath).filter(([path]) =>
          validPaths.has(path),
        ),
      ),
    }));

    const activeAlbumId = get().activeAlbumId;
    if (activeAlbumId) {
      if (activeAlbumId !== FAVORITES_ALBUM_ID && !albumsById[activeAlbumId]) {
        set({
          activeAlbumId: null,
          favoritesOnly: false,
          loadingAlbumId: null,
        });
      } else {
        get().autoExpandAncestors(activeAlbumId);
        if (activeAlbumId !== FAVORITES_ALBUM_ID) {
          const activeAlbum = albumsById[activeAlbumId];
          if (activeAlbum && !get().isAlbumLoaded(activeAlbumId)) {
            await get().loadAlbumMedia(activeAlbumId);
          }
        }
      }
    }

    await get().refreshFavoritesMap();
  },

  createAlbum: async (name: string, parentId?: AlbumId | null) => {
    const state = get();
    const rootDir =
      state.displayDecoy && state.decoyRoot ? state.decoyRoot : state.rootDir;
    if (!rootDir) return;
    const parentPath =
      parentId && get().albumsById[parentId]
        ? get().albumsById[parentId]!.path
        : rootDir;
    await _createAlbum(parentPath, name);
  },

  deleteAlbum: async (albumId: AlbumId) => {
    const album = get().albumsById[albumId];
    if (!album) return;
    const state = get();
    const rootDir =
      state.displayDecoy && state.decoyRoot ? state.decoyRoot : state.rootDir;
    if (!rootDir) return;
    await _deleteAlbum(album);
  },

  moveAlbum: async (albumId: AlbumId, newParentId: AlbumId | null) => {
    const album = get().albumsById[albumId];
    if (!album) return;
    const state = get();
    const rootDir =
      state.displayDecoy && state.decoyRoot ? state.decoyRoot : state.rootDir;
    if (!rootDir) return;
    try {
      const res = await _moveAlbum(rootDir, album, newParentId);
      const mapId = (id: AlbumId): AlbumId => {
        if (id === res.oldRelativePath) return res.newRelativePath;
        if (id.startsWith(`${res.oldRelativePath}/`)) {
          return res.newRelativePath + id.slice(res.oldRelativePath.length);
        }
        return id;
      };

      set((state) => {
        const albumMediasByPath = { ...state.albumMediasByPath };
        if (albumMediasByPath[res.oldPath]) {
          albumMediasByPath[res.newPath] = albumMediasByPath[res.oldPath]!;
          delete albumMediasByPath[res.oldPath];
        }
        const albumDuplicatesByPath = { ...state.albumDuplicatesByPath };
        if (albumDuplicatesByPath[res.oldPath]) {
          albumDuplicatesByPath[res.newPath] =
            albumDuplicatesByPath[res.oldPath]!;
          delete albumDuplicatesByPath[res.oldPath];
        }
        return {
          expandedAlbumIds: new Set(
            Array.from(state.expandedAlbumIds).map(mapId),
          ),
          manuallyExpandedAlbumIds: new Set(
            Array.from(state.manuallyExpandedAlbumIds).map(mapId),
          ),
          activeAlbumId:
            state.activeAlbumId && state.activeAlbumId !== FAVORITES_ALBUM_ID
              ? mapId(state.activeAlbumId)
              : state.activeAlbumId,
          selection: [],
          viewerIndex: null,
          albumMediasByPath,
          albumDuplicatesByPath,
        };
      });

      await get().hotRefresh();
      const activeId = get().activeAlbumId;
      if (
        activeId &&
        activeId !== FAVORITES_ALBUM_ID &&
        !get().albumsById[activeId]
      ) {
        set({
          activeAlbumId: null,
          favoritesOnly: false,
          loadingAlbumId: null,
          selection: [],
          viewerIndex: null,
        });
      }
      toast.success(`Moved "${album.name}"`);
    } catch (error) {
      const message =
        error instanceof Error && error.message
          ? error.message
          : "Failed to move album";
      toast.error(message);
      throw error;
    }
  },

  renameAlbum: async (albumId: AlbumId, newName: string) => {
    const album = get().albumsById[albumId];
    if (!album) return;
    const state = get();
    const rootDir =
      state.displayDecoy && state.decoyRoot ? state.decoyRoot : state.rootDir;
    if (!rootDir) return;
    const res = await _renameAlbum(rootDir, album, newName);
    const mapId = (id: AlbumId): AlbumId => {
      if (id === res.oldRelativePath) return res.newRelativePath;
      if (id.startsWith(`${res.oldRelativePath}/`)) {
        return res.newRelativePath + id.slice(res.oldRelativePath.length);
      }
      return id;
    };

    set((state) => {
      const albumMediasByPath = { ...state.albumMediasByPath };
      if (albumMediasByPath[res.oldPath]) {
        albumMediasByPath[res.newPath] = albumMediasByPath[res.oldPath]!;
        delete albumMediasByPath[res.oldPath];
      }
      const albumDuplicatesByPath = { ...state.albumDuplicatesByPath };
      if (albumDuplicatesByPath[res.oldPath]) {
        albumDuplicatesByPath[res.newPath] =
          albumDuplicatesByPath[res.oldPath]!;
        delete albumDuplicatesByPath[res.oldPath];
      }
      return {
        expandedAlbumIds: new Set(
          Array.from(state.expandedAlbumIds).map(mapId),
        ),
        manuallyExpandedAlbumIds: new Set(
          Array.from(state.manuallyExpandedAlbumIds).map(mapId),
        ),
        activeAlbumId:
          state.activeAlbumId && state.activeAlbumId !== FAVORITES_ALBUM_ID
            ? mapId(state.activeAlbumId)
            : state.activeAlbumId,
        selection: [],
        viewerIndex: null,
        albumMediasByPath,
        albumDuplicatesByPath,
      };
    });

    await get().hotRefresh();
    const activeId = get().activeAlbumId;
    if (
      activeId &&
      activeId !== FAVORITES_ALBUM_ID &&
      !get().albumsById[activeId]
    ) {
      set({
        activeAlbumId: null,
        favoritesOnly: false,
        loadingAlbumId: null,
        selection: [],
        viewerIndex: null,
      });
    }
  },
});
