"use client";

import { useEffect } from "react";
import type { MediaEntry } from "@/lib/types";

interface Opt {
  selection: Set<MediaEntry>;
  clearSelection: () => void;
  selectAll: () => void;
  viewer: {
    viewerIndex: number | null;
    next: () => void;
    prev: () => void;
    close: () => void;
  };
  lock: { locked: boolean; lock: () => void; unlock: () => void };
}

export function useKeyboardShortcuts({
  selection,
  clearSelection,
  selectAll,
  viewer,
  lock,
}: Opt) {
  useEffect(() => {
    const h = (e: KeyboardEvent) => {
      if (document.activeElement instanceof HTMLInputElement) return;
      const k = e.key.toLowerCase();
      if (lock.locked) {
        if (k === "u" || k === "г") lock.unlock();
        return;
      }
      if (k === "l" || k === "д") {
        lock.lock();
        return;
      }
      if (viewer.viewerIndex !== null) {
        if (e.key === "ArrowLeft") viewer.prev();
        if (e.key === "ArrowRight") viewer.next();
        if (e.key === "Escape") viewer.close();
        return;
      }
      if ((e.metaKey || e.ctrlKey) && (k === "a" || k === "ф")) {
        e.preventDefault();
        selectAll();
        return;
      }
      if ((e.metaKey || e.ctrlKey) && (k === "d" || k === "в")) {
        e.preventDefault();
        clearSelection();
        return;
      }
    };
    document.addEventListener("keydown", h);
    return () => document.removeEventListener("keydown", h);
  }, [selection, viewer, lock, clearSelection, selectAll]);
}
