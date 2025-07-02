"use client";

import { type ReactNode, useState } from "react";
import { useRootDir } from "../hooks/use-root-dir";
import { useAlbums } from "../hooks/use-albums";
import { useMedia } from "../hooks/use-media";
import { useSelection } from "../hooks/use-selection";
import { useDragDrop } from "../hooks/use-drag-drop";
import { useViewer } from "../hooks/use-viewer";
import { useKeyboardShortcuts } from "../hooks/use-keyboard-shortcuts";
import { useUpload } from "../hooks/use-upload";
import { GalleryContext, type SortDir, type SortKey } from "./gallery-context";
import { moveMedia } from "../fs/albumService";
import type { Album, MediaEntry } from "@/lib/types";
import { useLockscreen } from "../hooks/use-lockscreen";

export function GalleryProvider({ children }: { children: ReactNode }) {
  const { rootDir, pickDirectory } = useRootDir();
  const albumsState = useAlbums(rootDir);

  const [columns, setColumns] = useState(4);
  const [sortKey, setSortKey] = useState<SortKey>("shoot");
  const [sortDir, setSortDir] = useState<SortDir>("desc");

  const photosState = useMedia(albumsState.activeAlbum, 40, sortKey, sortDir);
  const sel = useSelection();
  const drag = useDragDrop(sel.selection);
  const viewer = useViewer(photosState.media.length);
  const lock = useLockscreen();

  const upload = useUpload(
    albumsState.albums,
    albumsState.refresh,
    albumsState.activeAlbum,
    photosState.addEntry,
  );

  const moveMediasToAlbum = async (t: Album, medias: MediaEntry[]) => {
    if (!t.handle || !medias.length) return;
    await moveMedia(albumsState.activeAlbum!, t, medias);
    await albumsState.refresh();
    if (albumsState.activeAlbum?.dirName === t.dirName) {
      for (const media of medias) {
        const fh = await t.handle.getFileHandle(media.file.name);
        const file = await fh.getFile();
        photosState.addEntry({
          file,
          url: media.url,
          unload: media.unload,
          meta: media.meta,
          handle: fh,
          thumb: media.thumb,
        });
      }
    } else if (albumsState.activeAlbum) {
      medias.forEach((e) => photosState.removeEntry(e));
    }
    sel.clear();
    drag.clear();
  };

  const moveSelectedToAlbum = async (t: Album) => {
    const medias = Array.from(sel.selection);
    if (!medias.length) return;
    await moveMediasToAlbum(t, medias);
  };

  const moveDraggedToAlbum = async (t: Album) => {
    const medias = drag.getDragged();
    if (!medias.length) return;
    await moveMediasToAlbum(t, medias);
  };

  const deleteMedias = async (medias: MediaEntry[]) => {
    const a = albumsState.activeAlbum;
    if (!a) return;
    for (const i of medias) {
      await a.handle.removeEntry(i.file.name);
      a.images = a.images.filter((n) => n !== i.file.name);
      photosState.removeEntry(i);
    }
    await albumsState.refresh();
  };

  const deleteMedia = async (i: MediaEntry) => {
    await deleteMedias([i]);
  };

  useKeyboardShortcuts({
    selection: sel.selection,
    clearSelection: sel.clear,
    selectAll: () => sel.selectAll(photosState.media),
    viewer,
    lock,
  });

  const ctx = {
    rootDir,
    pickDirectory,
    albums: albumsState.albums,
    activeAlbum: albumsState.activeAlbum,
    setActive: albumsState.setActive,
    createAlbum: albumsState.createAlbum,
    deleteAlbum: albumsState.deleteAlbum,
    media: photosState.media,
    loadMore: photosState.loadMore,
    columns,
    setColumns,
    sortKey,
    setSortKey,
    sortDir,
    setSortDir,
    selection: sel.selection,
    toggleSelect: sel.toggle,
    clearSelection: sel.clear,
    viewer,
    onDragStart: drag.onDragStart,
    getDragged: drag.getDragged,
    addFilesToAlbum: upload.addFilesToAlbum,
    uploadFilesToActive: upload.uploadFilesToActive,
    moveDraggedToAlbum,
    deleteMedias,
    deleteMedia,
    moveMediasToAlbum,
    moveSelectedToAlbum,
    locked: lock.locked,
    layout: photosState.layout,
    setLayout: photosState.setLayout,
  };

  return (
    <GalleryContext.Provider value={ctx}>{children}</GalleryContext.Provider>
  );
}
