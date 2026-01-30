import { type Store } from "@tauri-apps/plugin-store";
import { getStore } from "../fs/state";
import type { State } from "./types";

const STORAGE_KEY = "room237-storage";

type PersistedState = Pick<
  State,
  | "columns"
  | "sortKey"
  | "sortDir"
  | "rootDir"
  | "decoyRoot"
  | "contentProtected"
  | "language"
  | "layout"
>;

let storePromise: Promise<Store> | null = null;
let lastSavedState: PersistedState | null = null;
let saveTimeout: ReturnType<typeof setTimeout> | null = null;
let pendingState: PersistedState | null = null;

async function getTauriStore(): Promise<Store> {
  return (storePromise ??= getStore());
}

export async function loadPersistedState(): Promise<Partial<PersistedState> | null> {
  try {
    const store = await getTauriStore();
    const stored = await store.get<string>(STORAGE_KEY);

    if (!stored) {
      return null;
    }

    const parsed = JSON.parse(stored) as Partial<PersistedState>;
    const merged: PersistedState = {
      columns: parsed.columns ?? 4,
      sortKey: parsed.sortKey ?? "shoot",
      sortDir: parsed.sortDir ?? "desc",
      rootDir: parsed.rootDir ?? null,
      decoyRoot: parsed.decoyRoot ?? null,
      contentProtected: parsed.contentProtected ?? false,
      language: parsed.language ?? "en",
      layout: parsed.layout ?? "default",
    };
    lastSavedState = merged;
    return merged;
  } catch (error) {
    console.error("Failed to load persisted state:", error);
    return null;
  }
}

export async function savePersistedState(state: PersistedState): Promise<void> {
  if (
    lastSavedState &&
    JSON.stringify(lastSavedState) === JSON.stringify(state)
  ) {
    return;
  }

  pendingState = state;
  if (saveTimeout) {
    clearTimeout(saveTimeout);
  }

  saveTimeout = setTimeout(() => {
    void flushPersistedState();
  }, 300);
}

export function extractPersistedState(state: State): PersistedState {
  return {
    columns: state.columns,
    sortKey: state.sortKey,
    sortDir: state.sortDir,
    rootDir: state.rootDir,
    decoyRoot: state.decoyRoot,
    contentProtected: state.contentProtected,
    language: state.language,
    layout: state.layout,
  };
}

export async function flushPersistedState(): Promise<void> {
  const stateToSave = pendingState;
  pendingState = null;
  if (!stateToSave) {
    return;
  }
  if (saveTimeout) {
    clearTimeout(saveTimeout);
    saveTimeout = null;
  }
  try {
    const store = await getTauriStore();
    const serialized = JSON.stringify(stateToSave);
    await store.set(STORAGE_KEY, serialized);
    await store.save();
    lastSavedState = { ...stateToSave };
  } catch (error) {
    console.error("Failed to flush persisted state:", error);
  }
}
