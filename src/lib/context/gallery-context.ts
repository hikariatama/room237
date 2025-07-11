"use client";

import { createContext, useContext } from "react";
import type { LayoutType, MediaEntry } from "@/lib/types";
import type { Album } from "@/lib/types/album";

export type SortKey = "shoot" | "added" | "name";
export type SortDir = "asc" | "desc";

type Ctx = {
  rootDir: string | null;
  pickDirectory: () => Promise<void>;
  albumsReady: boolean;
  albums: Album[];
  activeAlbum: Album | null;
  setActive: (id: string) => void;
  createAlbum: (n: string) => Promise<void>;
  deleteAlbum: (a: Album) => Promise<void>;
  media: MediaEntry[];
  loadMore: () => void;
  isFullyLoaded: boolean;
  invalidateMedia: (name: string) => Promise<void>;
  columns: number;
  setColumns: (n: number) => void;
  sortKey: SortKey;
  setSortKey: (k: SortKey) => void;
  sortDir: SortDir;
  setSortDir: (d: SortDir) => void;
  selection: Set<MediaEntry>;
  toggleSelect: (i: MediaEntry, add: boolean) => void;
  clearSelection: () => void;
  viewer: {
    viewerIndex: number | null;
    open: (i: number) => void;
    close: () => void;
    next: () => void;
    prev: () => void;
  };
  onDragStart: (
    e: MouseEvent | TouchEvent | PointerEvent | React.DragEvent<Element>,
    i: MediaEntry,
  ) => void;
  getDragged: () => MediaEntry[];
  addFilesToAlbum: (a: Album, f: FileList | File[]) => Promise<void>;
  uploadFilesToActive: (f: FileList | File[]) => Promise<void>;
  moveDraggedToAlbum: (a: Album) => Promise<void>;
  deleteMedias: (medias: MediaEntry[]) => Promise<void>;
  deleteMedia: (i: MediaEntry) => Promise<void>;
  moveMediasToAlbum: (t: Album, medias: MediaEntry[]) => Promise<void>;
  moveSelectedToAlbum: (t: Album) => Promise<void>;
  locked: boolean;
  layout: LayoutType;
  setLayout: (l: LayoutType) => void;
  loadingAlbum: string | null;
  allowOpen: boolean;
  setAllowOpen: (allow: boolean) => void;
};

export const GalleryContext = createContext<Ctx>(null as unknown as Ctx);
export const useGallery = () => useContext(GalleryContext);
