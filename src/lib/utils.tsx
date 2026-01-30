import { clsx, type ClassValue } from "clsx";
import { twMerge } from "tailwind-merge";
import type { FileMeta, MediaEntry, OS } from "./types";
import { convertFileSrc, invoke } from "@tauri-apps/api/core";
import path from "path";
import type { DetachedMediaEntry } from "./types";
import { exists } from "@tauri-apps/plugin-fs";
import { TbBrandFinder } from "react-icons/tb";
import type { JSX } from "react";
import { IconFolder } from "@tabler/icons-react";
import type { FileManager } from "./fs/albumService";
import { GiDolphin } from "react-icons/gi";
import { SiGnome, SiPantheon } from "react-icons/si";

import { useEffect, useState } from "react";
import { toast } from "@/components/toaster";
import type { State } from "./stores/types";

export const cn = (...i: ClassValue[]) => twMerge(clsx(i));

export const isImage = (n: string) =>
  /\.(png|jpe?g|gif|bmp|webp|avif)$/i.test(n);
export const isVideo = (n: string) => /\.(mp4|webm|ogg)$/i.test(n);
export const isMedia = (n: string) => isImage(n) || isVideo(n);
export const clamp = (value: number, min: number, max: number) =>
  Math.min(Math.max(value, min), max);

export function splitName(name: string): {
  stem: string;
  ext: string;
  startCounter: number;
} {
  const ext = path.extname(name);
  const stemRaw = path.basename(name, ext);
  const match = /^(.*)_([0-9]+)$/.exec(stemRaw);
  if (match?.[1]) {
    return { stem: match[1], ext, startCounter: parseInt(match[2]!, 10) };
  }
  return { stem: stemRaw, ext, startCounter: 0 };
}

export async function nextAvailableName(
  dir: string,
  stem: string,
  ext: string,
  startCounter = 0,
): Promise<string> {
  let counter = startCounter;
  let candidate = counter > 0 ? `${stem}_${counter}${ext}` : `${stem}${ext}`;
  while (await exists(path.join(dir, candidate))) {
    counter += 1;
    candidate = `${stem}_${counter}${ext}`;
  }
  return candidate;
}

export const loadImageDims = (f: File) =>
  new Promise<{ w: number; h: number }>((res, rej) => {
    const i = new Image();
    i.onload = () => res({ w: i.naturalWidth, h: i.naturalHeight });
    i.onerror = rej;
    i.src = URL.createObjectURL(f);
  });

export function createStackPreview(medias: MediaEntry[]): HTMLDivElement {
  const c = document.createElement("div");
  c.style.position = "relative";
  c.style.width = "80px";
  c.style.height = "80px";
  c.style.userSelect = "none";
  c.style.pointerEvents = "none";
  c.style.borderRadius = "0.5rem";
  const cnt = Math.min(medias.length, 3);
  for (let i = 0; i < cnt; i += 1) {
    const el = document.createElement("img");
    el.src = medias[i]!.thumb;
    el.style.width = "80px";
    el.style.height = "80px";
    el.style.objectFit = "cover";
    el.style.position = "absolute";
    el.style.top = `${8 * i + 16}px`;
    el.style.left = `${8 * i + 16}px`;
    el.style.border = "1px solid rgba(0,0,0,0.25)";
    el.style.borderRadius = "0.5rem";
    el.draggable = false;
    c.appendChild(el);
  }
  if (medias.length > 3) {
    const o = document.createElement("div");
    o.textContent = String(medias.length);
    o.style.position = "absolute";
    o.style.marginLeft = "32px";
    o.style.marginTop = "32px";
    o.style.width = "80px";
    o.style.height = "80px";
    o.style.display = "flex";
    o.style.alignItems = "center";
    o.style.justifyContent = "center";
    o.style.backdropFilter = "blur(4px)";
    o.style.background = "rgba(0,0,0,0.4)";
    o.style.color = "white";
    o.style.fontWeight = "bold";
    o.style.fontSize = "20px";
    o.style.borderRadius = "0.5rem";
    c.appendChild(o);
  }
  return c;
}

export function animateFly(
  medias: MediaEntry[],
  rects: (DOMRect | undefined)[],
  startX: number,
  startY: number,
): void {
  const flyers: HTMLImageElement[] = [];

  medias.forEach((m, i) => {
    const r = rects[i];
    if (!r) return;

    const el = document.createElement("img");
    el.src = m.thumb;
    Object.assign(el.style, {
      position: "fixed",
      top: `${r.top}px`,
      left: `${r.left}px`,
      width: `${r.width}px`,
      height: `${r.height}px`,
      objectFit: "cover",
      borderRadius: "1rem",
      zIndex: "9999",
      pointerEvents: "none",
      userSelect: "none",
      willChange: "transform",
    });
    document.body.appendChild(el);
    flyers.push(el);
  });

  const itemPositions = medias.map((m, i) => {
    const r = rects[i];
    if (!r) return { x: 0, y: 0 };
    return {
      x: r.left + r.width / 2,
      y: r.top + r.height / 2,
    };
  });
  let curScale = 1;
  const ease = 0.15;
  let targetX = startX;
  let targetY = startY;

  let raf = requestAnimationFrame(function loop() {
    itemPositions.forEach((pos) => {
      pos.x += (targetX - pos.x) * ease;
      pos.y += (targetY - pos.y) * ease;
    });
    curScale -= (curScale - 0.2) * ease;

    flyers.forEach((el, i) => {
      const r = rects[i];
      if (!r) return;
      el.style.transform = `
        translate3d(
          ${itemPositions[i]!.x - r.left - r.width / 2}px,
          ${itemPositions[i]!.y - r.top - r.height / 2}px,
          0
        )
        scale(${curScale})
      `;
    });

    raf = requestAnimationFrame(loop);
  });

  const move = (e: PointerEvent | DragEvent) => {
    targetX = e.clientX;
    targetY = e.clientY;
  };
  window.addEventListener("pointermove", move);
  window.addEventListener("dragover", move);

  requestAnimationFrame(() => {
    setTimeout(cleanup, 400);
  });

  function cleanup() {
    cancelAnimationFrame(raf);
    flyers.forEach((f) => f.remove());
    window.removeEventListener("pointermove", move);
    window.removeEventListener("dragover", move);
  }
}

export function unpackFileMeta(packed: string): FileMeta {
  const p = BigInt(packed);
  const added = Number(p & ((1n << 40n) - 1n));
  const shoot = Number((p >> 40n) & ((1n << 40n) - 1n));
  const width = Number((p >> 80n) & ((1n << 20n) - 1n));
  const height = Number((p >> 100n) & ((1n << 20n) - 1n));

  const isImage = (p & (1n << 120n)) !== 0n;
  const isVideo = (p & (1n << 121n)) !== 0n;
  const hasA = (p & (1n << 122n)) !== 0n;
  const hasS = (p & (1n << 123n)) !== 0n;
  const hasW = (p & (1n << 124n)) !== 0n;
  const hasH = (p & (1n << 125n)) !== 0n;

  return {
    added: hasA ? added : null,
    shoot: hasS ? shoot : null,
    isImage,
    isVideo,
    width: hasW ? width : undefined,
    height: hasH ? height : undefined,
  };
}

export function attachMediaEntry(
  albumPath: string,
  entry: DetachedMediaEntry,
  albumName: string,
  albumId: string,
): MediaEntry {
  return {
    url: convertFileSrc(path.join(albumPath, entry.name)),
    thumb: convertFileSrc(
      path.join(albumPath, ".room237-thumb", `${entry.name}.webp`),
    ),
    meta: unpackFileMeta(entry.meta),
    name: entry.name,
    path: path.join(albumPath, entry.name),
    favorite: entry.favorite ?? false,
    albumId,
    albumPath,
    albumName,
  } satisfies MediaEntry;
}

export const copyFiles = async (items: MediaEntry[]): Promise<void> => {
  if (!items || items.length === 0) throw new Error("No items selected");

  try {
    const filePaths = items.map((item) => item.path);

    try {
      await invoke<void>("set_clipboard_files", { paths: filePaths });

      const count = items.length;
      if (count === 1) {
        toast.success("File copied to clipboard!");
      } else {
        toast.success(`${count} files copied to clipboard!`);
      }
    } catch (nativeError) {
      console.warn(
        "Native clipboard failed, falling back to text:",
        nativeError,
      );

      const pathsText = filePaths.join("\n");
      await navigator.clipboard.writeText(pathsText);

      const count = items.length;
      if (count === 1) {
        toast.success("File path copied to clipboard!");
      } else {
        toast.success(`${count} file paths copied to clipboard!`);
      }
    }
  } catch (error) {
    console.error("Failed to copy files:", error);
    toast.error("Failed to copy to clipboard");
    throw error;
  }
};

type IdleDeadlineLike = { timeRemaining(): number };
type IdleCb = (deadline: IdleDeadlineLike) => void;

export const requestIdle = (cb: IdleCb): number => {
  const w = window as typeof window & {
    requestIdleCallback?: (cb: IdleCb) => number;
  };
  if (w.requestIdleCallback) return w.requestIdleCallback(cb);
  return window.setTimeout(() => cb({ timeRemaining: () => 0 }), 1);
};

export const cancelIdle = (id: number): void => {
  const w = window as typeof window & {
    cancelIdleCallback?: (id: number) => void;
  };
  if (w.cancelIdleCallback) w.cancelIdleCallback(id);
  else window.clearTimeout(id);
};

export const getFileManagerIcon = (name: FileManager): JSX.Element => {
  switch (name) {
    case "Finder":
      return <TbBrandFinder className="h-4 w-4" />;
    case "Dolphin":
      return <GiDolphin className="h-4 w-4" />;
    case "GNOME Files":
      return <SiGnome className="h-4 w-4" />;
    case "Pantheon Files":
      return <SiPantheon className="h-4 w-4" />;
    default:
      return <IconFolder className="h-4 w-4" />;
  }
};

// eslint-disable-next-line @typescript-eslint/no-explicit-any
export const debounce = <F extends (...args: any[]) => void>(
  func: F,
): ((...args: Parameters<F>) => void) & { cancel: () => void } => {
  let rafId: number | null = null;
  let lastArgs: Parameters<F> | null = null;

  const cancel = () => {
    if (rafId != null) cancelAnimationFrame(rafId);
    rafId = null;
    lastArgs = null;
  };

  const debounced = ((...args: Parameters<F>) => {
    lastArgs = args;
    if (rafId != null) return;

    rafId = requestAnimationFrame(() => {
      rafId = null;
      const a = lastArgs;
      lastArgs = null;
      if (a) func(...a);
    });
  }) as ((...args: Parameters<F>) => void) & { cancel: () => void };

  debounced.cancel = cancel;

  return debounced;
};

export const useOS = (): OS => {
  const [os, setOS] = useState<OS>("other");

  useEffect(() => {
    const { userAgent } = window.navigator;

    if (userAgent.includes("Mac")) {
      setOS("macos");
    } else {
      setOS("other");
    }
  }, []);

  return os;
};

export const extractItemFromState = ({
  state,
  path,
  index,
}:
  | { state: State; path: string; index?: never }
  | { state: State; index: number; path?: never }) => {
  if (!state.activeAlbumId) return undefined;

  if (state.activeAlbumId === "Favorites") {
    const medias = state.albumMediasByPath.Favorites;
    if (!medias) return undefined;
    const sorted = state.favoritesAlbum?.getSortedMediaMap(
      medias,
      state.sortKey,
      state.sortDir,
      state.favoritesOnly,
      state.randomSeed,
    );
    return path
      ? sorted?.[path]
      : Object.values(sorted ?? {}).find((m) => m.index === index);
  }

  const activeAlbum = state.albumsById[state.activeAlbumId];
  if (!activeAlbum) return undefined;
  const medias = state.albumMediasByPath[activeAlbum.path];
  if (!medias) return undefined;
  const sorted = activeAlbum.getSortedMediaMap(
    medias,
    state.sortKey,
    state.sortDir,
    state.favoritesOnly,
    state.randomSeed,
  );
  return path
    ? sorted?.[path]
    : Object.values(sorted ?? {}).find((m) => m.index === index);
};
