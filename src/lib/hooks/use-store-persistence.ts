import { exists } from "@tauri-apps/plugin-fs";
import { useCallback, useEffect, useRef, useState } from "react";
import { getCurrentWindow } from "@tauri-apps/api/window";
import { useRoom237 } from "../stores";
import {
  extractPersistedState,
  loadPersistedState,
  savePersistedState,
  flushPersistedState,
} from "../stores/persistence";

export function useStorePersistence() {
  const isInitialized = useRef(false);
  const isLoading = useRef(false);
  const columns = useRoom237((state) => state.columns);
  const sortKey = useRoom237((state) => state.sortKey);
  const sortDir = useRoom237((state) => state.sortDir);
  const rootDir = useRoom237((state) => state.rootDir);
  const decoyRoot = useRoom237((state) => state.decoyRoot);
  const contentProtected = useRoom237((state) => state.contentProtected);
  const privacyEnabled = useRoom237((state) => state.privacyEnabled);
  const language = useRoom237((state) => state.language);
  const layout = useRoom237((state) => state.layout);
  const [ready, setReady] = useState(false);

  const validateRoot = useCallback(async (dir: string | null) => {
    if (!dir) return false;
    if (!(await exists(dir))) return false;
    return true;
  }, []);

  useEffect(() => {
    if (isLoading.current || isInitialized.current) return;

    isLoading.current = true;
    void (async () => {
      try {
        const persisted = await loadPersistedState();
        if (persisted) {
          const {
            setColumns,
            setSortKey,
            setSortDir,
            setRootDir,
            setDecoyRoot,
            setContentProtected,
            setLanguage: setLanguageState,
            setLayout,
          } = useRoom237.getState();

          if (persisted.columns !== undefined) {
            setColumns(persisted.columns);
          }
          if (persisted.sortKey !== undefined) {
            setSortKey(persisted.sortKey);
          }
          if (persisted.sortDir !== undefined) {
            setSortDir(persisted.sortDir);
          }
          if (persisted.rootDir !== undefined) {
            if (!(await validateRoot(persisted.rootDir))) {
              setRootDir(null);
            } else {
              setRootDir(persisted.rootDir);
            }
          }
          if (persisted.decoyRoot !== undefined) {
            setDecoyRoot(persisted.decoyRoot);
          }
          if (persisted.contentProtected !== undefined) {
            setContentProtected(persisted.contentProtected);
          }
          if (persisted.language !== undefined) {
            setLanguageState(persisted.language);
          } else {
            const navLang =
              typeof navigator !== "undefined" && navigator.language
                ? navigator.language.toLowerCase()
                : "";
            setLanguageState(navLang.startsWith("ru") ? "ru" : "en");
          }
          if (persisted.layout !== undefined) {
            setLayout(persisted.layout);
          }
        }
      } finally {
        isInitialized.current = true;
        isLoading.current = false;
        setReady(true);
      }
    })();
  }, [validateRoot]);

  useEffect(() => {
    if (!isInitialized.current) return;

    const state = useRoom237.getState();
    const persistedState = extractPersistedState(state);
    void savePersistedState(persistedState);
  }, [
    columns,
    sortKey,
    sortDir,
    rootDir,
    decoyRoot,
    contentProtected,
    language,
    layout,
  ]);

  useEffect(() => {
    const window = getCurrentWindow();
    const effectiveContentProtected = privacyEnabled && contentProtected;
    window.setContentProtected(effectiveContentProtected).catch((err) => {
      console.error("Failed to set content protection:", err);
    });
  }, [contentProtected, privacyEnabled]);

  useEffect(() => {
    const flush = () => {
      void flushPersistedState();
    };
    const onVisibilityChange = () => {
      if (document.visibilityState === "hidden") {
        flush();
      }
    };
    window.addEventListener("beforeunload", flush);
    window.addEventListener("pagehide", flush);
    document.addEventListener("visibilitychange", onVisibilityChange);
    return () => {
      flush();
      window.removeEventListener("beforeunload", flush);
      window.removeEventListener("pagehide", flush);
      document.removeEventListener("visibilitychange", onVisibilityChange);
    };
  }, []);

  return ready;
}
