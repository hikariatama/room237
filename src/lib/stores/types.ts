import type { StateCreator } from "zustand";
import type { LayoutType, MediaEntry } from "@/lib/types";
import type { Album, AlbumId, AlbumNode } from "@/lib/types/album";

export type Language = "en" | "ru";

export type SortKey = "shoot" | "added" | "name" | "random";
export type SortDir = "asc" | "desc";

export type DragHoverHint = {
  albumId: AlbumId;
  albumName: string;
  text: string;
};

export type AlbumsSlice = {
  rootDir: string | null;
  albumsReady: boolean;
  albumsById: Record<AlbumId, Album>;
  albumMediasByPath: Record<string, MediaEntry[]>;
  albumDuplicatesByPath: Record<string, string[][]>;
  albumTree: AlbumNode[];
  expandedAlbumIds: Set<AlbumId>;
  manuallyExpandedAlbumIds: Set<AlbumId>;
  activeAlbumId: AlbumId | null;
  loadingAlbumId: AlbumId | null;
  duplicatesAvailable: boolean;
  duplicatesLoading: boolean;
  setDuplicatesAvailable: (duplicatesAvailable: boolean) => void;
  setDuplicatesLoading: (loading: boolean) => void;
  setRootDir: (dir: string | null) => void;
  setAlbumsReady: (ready: boolean) => void;
  setAlbums: (albums: Record<AlbumId, Album>, tree: AlbumNode[]) => void;
  setAlbumTree: (tree: AlbumNode[]) => void;
  setExpandedAlbumIds: (ids: Set<AlbumId>) => void;
  setManuallyExpandedAlbumIds: (ids: Set<AlbumId>) => void;
  expandAlbum: (id: AlbumId) => void;
  collapseAlbum: (id: AlbumId) => void;
  markManualExpand: (id: AlbumId) => void;
  markManualCollapse: (id: AlbumId) => void;
  collapseAutoExpandedExcept: (keepId: AlbumId | null) => void;
  setActiveAlbumId: (albumId: AlbumId | null) => void;
  setLoadingAlbumId: (id: AlbumId | null) => void;
  isAlbumLoaded: (albumId: AlbumId) => boolean;
  loadAlbumMedia: (
    albumId: AlbumId,
    options?: { force?: boolean },
  ) => Promise<MediaEntry[]>;
  loadAlbumDuplicates: (
    albumId: AlbumId,
    options?: { force?: boolean },
  ) => Promise<string[][]>;
  triggerAlbumUpdate: (albumId: AlbumId) => void;
  setActive: (albumId: AlbumId) => Promise<void>;
  autoExpandAncestors: (albumId: AlbumId | null) => void;
  hardRefresh: () => Promise<void>;
  hotRefresh: () => Promise<void>;
  createAlbum: (name: string, parentId?: AlbumId | null) => Promise<void>;
  deleteAlbum: (albumId: AlbumId) => Promise<void>;
  renameAlbum: (albumId: AlbumId, newName: string) => Promise<void>;
  moveAlbum: (albumId: AlbumId, newParentId: AlbumId | null) => Promise<void>;
};

export type MediaSlice = {
  urlCache: Map<string, string>;
};

export type FavoritesSlice = {
  favoritesMap: Record<AlbumId, MediaEntry[]>;
  favoriteFilters: Record<AlbumId, boolean>;
  favoritesAlbum: Album | null;
  refreshFavoritesMap: () => Promise<void>;
  toggleFavoritesOnly: (albumId: AlbumId) => void;
  getFavoritesAlbum: () => Album | null;
};

export type UISlice = {
  columns: number;
  sortKey: SortKey;
  sortDir: SortDir;
  language: Language;
  selection: MediaEntry[];
  draggedItems: MediaEntry[];
  layout: LayoutType;
  showDuplicates: boolean;
  favoritesOnly: boolean;
  randomSeed: number;
  fileManagerName: string | null;
  viewerIndex: number | null;
  dragHoverHint: DragHoverHint | null;
  batchOperationInProgress: boolean;
  setBatchOperationInProgress: (inProgress: boolean) => void;
  setDraggedItems: (items: MediaEntry[]) => void;
  clearDraggedItems: () => void;
  setDragHoverHint: (hint: DragHoverHint) => void;
  clearDragHoverHint: () => void;
  setColumns: (n: number) => void;
  setSortKey: (k: SortKey) => void;
  setSortDir: (d: SortDir) => void;
  setLanguage: (language: Language) => void;
  setLayout: (l: LayoutType) => void;
  setFileManagerName: (name: string | null) => void;
  toggleSelection: (media: MediaEntry, additive: boolean) => void;
  selectRange: (media: MediaEntry) => void;
  clearSelection: () => void;
  selectAll: () => void;
  setShowDuplicates: (show: boolean) => void;
  setFavoritesOnly: (favoritesOnly: boolean) => void;
  setRandomSeed: (randomSeed: number) => void;
  openViewer: (index: number) => void;
  closeViewer: () => void;
  nextViewer: () => void;
  prevViewer: () => void;
  toggleFavorite: (media: MediaEntry) => Promise<void>;
  setFavorite: (media: MediaEntry, favorite: boolean) => Promise<void>;
  deleteMedias: (medias: MediaEntry[]) => Promise<void>;
  deleteMedia: (media: MediaEntry) => Promise<void>;
  moveMediasToAlbum: (album: Album, medias: MediaEntry[]) => Promise<void>;
  patchMediaDates: (medias: MediaEntry[], timestamp: number) => () => void;
  updateMediaDates: (medias: MediaEntry[], timestamp: number) => Promise<void>;
  addFilesToAlbum: (album: Album, files: FileList | File[]) => Promise<void>;
  uploadFilesToActive: (files: FileList | File[]) => Promise<void>;
  moveDraggedToAlbum: (album: Album) => Promise<void>;
  moveSelectedToAlbum: (albumId: AlbumId) => Promise<void>;
};

export type DebugSlice = {
  isLogger: boolean;
  isDebug: boolean;
  setIsLogger: (open: boolean) => void;
  setIsDebug: (open: boolean) => void;
};

export type DecoySlice = {
  isUnfocused: boolean;
  setIsUnfocused: (isUnfocused: boolean) => void;
  locked: boolean;
  setLocked: (locked: boolean) => void;
  allowOpen: boolean;
  setAllowOpen: (allow: boolean) => void;
  decoyRoot: string | null;
  setDecoyRoot: (root: string | null) => void;
  displayDecoy: boolean;
  setDisplayDecoy: (display: boolean) => void;
  contentProtected: boolean;
  setContentProtected: (contentProtected: boolean) => void;
  privacyEnabled: boolean;
  setPrivacyEnabled: (enabled: boolean) => void;
  lockscreenEnabled: boolean;
  setLockscreenEnabled: (enabled: boolean) => void;
  confirmOpenEnabled: boolean;
  setConfirmOpenEnabled: (enabled: boolean) => void;
};

export type State = AlbumsSlice &
  UISlice &
  DebugSlice &
  DecoySlice &
  MediaSlice &
  FavoritesSlice;

export type CustomStateCreator<T> = StateCreator<State, [], [], T>;
