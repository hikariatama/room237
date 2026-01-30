"use client";

import { useCallback, useEffect, useRef } from "react";
import { useRoom237 } from "../stores";
import { copyFiles, extractItemFromState, useOS } from "../utils";
import { useViewer } from "./use-viewer";

const compareKey = (keyRaw: string, keyEn: string) => {
  const enToRu: Record<string, string> = {
    q: "й",
    w: "ц",
    e: "у",
    r: "к",
    t: "е",
    y: "н",
    u: "г",
    i: "ш",
    o: "щ",
    p: "з",
    "[": "х",
    "]": "ъ",
    a: "ф",
    s: "ы",
    d: "в",
    f: "а",
    g: "п",
    h: "р",
    j: "о",
    k: "л",
    l: "д",
    ";": "ж",
    "'": "э",
    z: "я",
    x: "ч",
    c: "с",
    v: "м",
    b: "и",
    n: "т",
    m: "ь",
    ",": "б",
    ".": "ю",
    "/": ".",
    "`": "ё",
  };
  const key = keyRaw.toLowerCase();
  if (key === keyEn) return true;
  if (enToRu[key] === keyEn) return true;
  return false;
};

export function useKeyboardShortcuts() {
  const os = useOS();
  const selection = useRoom237((state) => state.selection);
  const clearSelection = useRoom237((state) => state.clearSelection);
  const selectAll = useRoom237((state) => state.selectAll);
  const locked = useRoom237((state) => state.locked);
  const setLocked = useRoom237((state) => state.setLocked);
  const lockscreenEnabled = useRoom237((state) => state.lockscreenEnabled);
  const setDisplayDecoy = useRoom237((state) => state.setDisplayDecoy);
  const decoyRoot = useRoom237((state) => state.decoyRoot);
  const hotRefresh = useRoom237((state) => state.hotRefresh);

  const viewer = useViewer();

  const isDebug = useRoom237((state) => state.isDebug);
  const setIsDebug = useRoom237((state) => state.setIsDebug);

  const keySequence = useRef("");
  const sequenceTimer = useRef<NodeJS.Timeout | null>(null);
  const inactivityTimer = useRef<NodeJS.Timeout | null>(null);

  const resetInactivityTimer = useCallback(() => {
    if (!lockscreenEnabled) return;
    if (inactivityTimer.current) clearTimeout(inactivityTimer.current);
    if (!locked) {
      inactivityTimer.current = setTimeout(
        () => {
          setLocked(true);
        },
        5 * 60 * 1000,
      );
    }
  }, [locked, lockscreenEnabled, setLocked]);

  useEffect(() => {
    if (!lockscreenEnabled && inactivityTimer.current) {
      clearTimeout(inactivityTimer.current);
      inactivityTimer.current = null;
    }
  }, [lockscreenEnabled]);

  useEffect(() => {
    const h = (e: KeyboardEvent) => {
      resetInactivityTimer();

      const isMeta = os === "macos" ? e.metaKey : e.ctrlKey;

      if (document.activeElement instanceof HTMLInputElement) return;
      const k = e.key.toLowerCase();

      keySequence.current += k;

      if (sequenceTimer.current) clearTimeout(sequenceTimer.current);
      sequenceTimer.current = setTimeout(() => {
        keySequence.current = "";
      }, 2000);

      if (keySequence.current.includes("venari")) {
        setIsDebug(true);
        keySequence.current = "";
        return;
      }

      if (lockscreenEnabled && locked) {
        if (k === "u" || k === "г") {
          if (e.metaKey || e.ctrlKey) {
            setDisplayDecoy(true);
            if (decoyRoot) {
              void hotRefresh();
            }
          }
          setLocked(false);
        }
        return;
      }
      if (lockscreenEnabled && compareKey(k, "l")) {
        setLocked(true);
        return;
      }
      if (viewer.viewerIndex !== null) {
        if (e.key === "ArrowLeft") viewer.prev();
        if (e.key === "ArrowRight") viewer.next();
        if (isMeta && compareKey(k, "c")) {
          const state = useRoom237.getState();
          const item = extractItemFromState({
            state,
            index: viewer.viewerIndex,
          });
          if (item) {
            e.preventDefault();
            void copyFiles([item]);
          }
        }
        return;
      }
      if (isMeta && compareKey(k, "c")) {
        if (selection.length > 0) {
          e.preventDefault();
          void copyFiles(selection);
          return;
        }
      }
      if ((os === "macos" && isMeta && k === "backspace") || k === "delete") {
        if (selection.length > 0) {
          e.preventDefault();
          void useRoom237.getState().deleteMedias(selection);
          return;
        }
      }
      if (isMeta && compareKey(k, "a")) {
        e.preventDefault();
        selectAll();
        return;
      }
      if (isMeta && compareKey(k, "d")) {
        e.preventDefault();
        clearSelection();
        return;
      }

      if (k === "escape" && isDebug) {
        setIsDebug(false);
        return;
      }
    };

    const handleMouseActivity = () => {
      resetInactivityTimer();
    };

    document.addEventListener("keydown", h);
    document.addEventListener("mousemove", handleMouseActivity);
    document.addEventListener("click", handleMouseActivity);

    resetInactivityTimer();

    return () => {
      document.removeEventListener("keydown", h);
      document.removeEventListener("mousemove", handleMouseActivity);
      document.removeEventListener("click", handleMouseActivity);
      if (sequenceTimer.current) clearTimeout(sequenceTimer.current);
      if (inactivityTimer.current) clearTimeout(inactivityTimer.current);
    };
  }, [
    selection,
    viewer,
    clearSelection,
    selectAll,
    isDebug,
    setIsDebug,
    resetInactivityTimer,
    locked,
    setLocked,
    setDisplayDecoy,
    decoyRoot,
    hotRefresh,
    lockscreenEnabled,
    os,
  ]);
}
