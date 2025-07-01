import { clsx, type ClassValue } from "clsx";
import { twMerge } from "tailwind-merge";
import type { MediaEntry } from "./types";

export const cn = (...i: ClassValue[]) => twMerge(clsx(i));

export const isImage = (n: string) =>
  /\.(png|jpe?g|gif|bmp|webp|avif)$/i.test(n);
export const isVideo = (n: string) => /\.(mp4|webm|ogg)$/i.test(n);
export const isMedia = (n: string) => isImage(n) || isVideo(n);

export const loadImageDims = (f: File) =>
  new Promise<{ w: number; h: number }>((res, rej) => {
    const i = new Image();
    i.onload = () => res({ w: i.naturalWidth, h: i.naturalHeight });
    i.onerror = rej;
    i.src = URL.createObjectURL(f);
  });

export function masonry(medias: MediaEntry[], columns: number): MediaEntry[][] {
  const cols: { h: number; imgs: MediaEntry[] }[] = Array.from(
    { length: columns },
    () => ({ h: 0, imgs: [] }),
  );
  for (const media of medias) {
    const t = cols.reduce((a, b) => (a.h <= b.h ? a : b));
    const r = (media.meta.height ?? 1) / (media.meta.width ?? 1);
    t.imgs.push(media);
    t.h += r;
  }
  return cols.map((c) => c.imgs);
}

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
  x: number,
  y: number,
): void {
  const flyers: HTMLImageElement[] = [];
  medias.forEach((media, i) => {
    const r = rects[i];
    if (!r) return;
    const f = document.createElement("img");
    f.src = media.thumb;
    f.style.position = "fixed";
    f.style.top = `${r.top}px`;
    f.style.left = `${r.left}px`;
    f.style.width = `${r.width}px`;
    f.style.height = `${r.height}px`;
    f.style.objectFit = "cover";
    f.style.borderRadius = "1rem";
    f.style.zIndex = "9999";
    f.style.pointerEvents = "none";
    f.style.userSelect = "none";
    f.style.transition = "transform 0.2s linear, opacity 0.4s ease";
    document.body.appendChild(f);
    flyers.push(f);
  });
  let tx = x;
  let ty = y;
  const upd = () => {
    flyers.forEach((f, i) => {
      const r = rects[i];
      if (!r) return;
      f.style.transform = `translate(${tx - r.left - r.width / 2}px, ${ty - r.top - r.height / 2}px) scale(0.2)`;
    });
  };
  requestAnimationFrame(upd);
  const pm = (e: PointerEvent) => {
    tx = e.clientX;
    ty = e.clientY;
    upd();
  };
  const dm = (e: DragEvent) => {
    tx = e.clientX;
    ty = e.clientY;
    upd();
  };
  window.addEventListener("pointermove", pm);
  window.addEventListener("dragover", dm);
  setTimeout(() => {
    flyers.forEach((f) => (f.style.opacity = "0"));
    setTimeout(() => {
      flyers.forEach((f) => f.remove());
      window.removeEventListener("pointermove", pm);
      window.removeEventListener("dragover", dm);
    }, 400);
  }, 1);
}
