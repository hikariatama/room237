/* eslint-disable @next/next/no-img-element */
"use client";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import {
  Popover,
  PopoverContent,
  PopoverTrigger,
} from "@/components/ui/popover";
import { ScrollArea } from "@/components/ui/scroll-area";
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from "@/components/ui/select";
import { Slider } from "@/components/ui/slider";
import { Toggle } from "@/components/ui/toggle";
import { cn } from "@/lib/utils";
import { AnimatePresence, motion } from "framer-motion";
import {
  ArrowDownAZ,
  ArrowLeft,
  ArrowRight,
  ArrowUpAZ,
  CircleX,
  EyeOff,
  FolderOpen,
  Loader2,
  Plus,
  SendToBack,
  Trash2,
  X,
} from "lucide-react";
import React, {
  useCallback,
  useEffect,
  useRef,
  useState,
  type DragEvent as ReactDragEvent,
  type MouseEvent as ReactMouseEvent,
} from "react";
import { isHeic, heicTo } from "heic-to";
import { Toaster, toast } from "sonner";

type FileMeta = {
  name?: string;
  added?: number;
  width?: number;
  height?: number;
};

interface ImageEntry {
  file: File;
  url: string;
  meta: FileMeta;
  handle: FileSystemFileHandle;
  deleted?: boolean;
}

interface Album {
  dirName: string;
  name: string;
  meta: FileMeta;
  thumb: string | null;
  images: string[];
  handle: FileSystemDirectoryHandle;
}

class MetadataManager {
  private static DIR_META = "._meta.json";
  static async readDirMeta(dir: FileSystemDirectoryHandle): Promise<FileMeta> {
    try {
      const metaHandle = await dir.getFileHandle(this.DIR_META);
      const metaFile = await metaHandle.getFile();
      return JSON.parse(await metaFile.text()) as FileMeta;
    } catch {
      return {} as FileMeta;
    }
  }
  static async writeDirMeta(
    dir: FileSystemDirectoryHandle,
    meta: FileMeta,
  ): Promise<void> {
    const fileHandle = await dir.getFileHandle(this.DIR_META, { create: true });
    const writable = await fileHandle.createWritable();
    await writable.write(JSON.stringify(meta));
    await writable.close();
  }
  static async readFileMeta(
    dir: FileSystemDirectoryHandle,
    fileName: string,
  ): Promise<FileMeta> {
    const base = fileName.replace(/\.[^.]+$/, "");
    try {
      const metaHandle = await dir.getFileHandle(`.${base}.json`);
      const metaFile = await metaHandle.getFile();
      return JSON.parse(await metaFile.text()) as FileMeta;
    } catch {
      return {} as FileMeta;
    }
  }
  static async writeFileMeta(
    dir: FileSystemDirectoryHandle,
    fileName: string,
    meta: FileMeta,
  ): Promise<void> {
    const base = fileName.replace(/\.[^.]+$/, "");
    const metaHandle = await dir.getFileHandle(`.${base}.json`, {
      create: true,
    });
    const writable = await metaHandle.createWritable();
    await writable.write(JSON.stringify(meta));
    await writable.close();
  }
}

interface AlbumItemProps {
  album: Album;
  active: boolean;
  highlighted: boolean;
  onClick: () => void;
  onDropExternal: (files: FileList) => void;
  onDropInternal: () => void;
  onDragOver: () => void;
}

const AlbumItem: React.FC<AlbumItemProps> = ({
  album,
  active,
  highlighted,
  onClick,
  onDropExternal,
  onDropInternal,
  onDragOver,
}) => {
  const handleDrop = (e: ReactDragEvent<HTMLDivElement>): void => {
    e.preventDefault();
    if (e.dataTransfer?.files?.length) onDropExternal(e.dataTransfer.files);
    else onDropInternal();
  };

  return (
    <motion.div
      exit={{ opacity: 0, scale: 0.8, transition: { duration: 0.1 } }}
      onClick={onClick}
      onDragEnter={onDragOver}
      onDragOver={(e) => {
        e.preventDefault();
        onDragOver();
      }}
      onDrop={handleDrop as unknown as React.DragEventHandler<HTMLDivElement>}
      className={cn(
        "relative mb-1 flex cursor-pointer items-center gap-2 rounded-lg border-2 py-1 pr-3 pl-2 transition-colors select-none",
        active ? "bg-white/10" : "hover:bg-white/5",
        highlighted ? "border-primary" : "border-transparent",
      )}
    >
      <div className="relative size-7 flex-shrink-0 overflow-hidden rounded-md bg-white/10">
        {album.thumb && (
          <img
            src={album.thumb}
            alt="thumb"
            className="h-full w-full object-cover blur-md"
          />
        )}
        <div className="absolute inset-0 flex items-center justify-center bg-black/30">
          <EyeOff className="text-foreground size-3" />
        </div>
      </div>
      <span className="flex-1 truncate text-sm">{album.name}</span>
      <span className="text-muted-foreground text-sm">
        {album.images.length}
      </span>
    </motion.div>
  );
};

interface MasonryImageProps {
  img: ImageEntry;
  selected: boolean;
  onSelectToggle: (img: ImageEntry, additive: boolean) => void;
  onDragStart: (
    e: MouseEvent | TouchEvent | PointerEvent | ReactDragEvent,
    img: ImageEntry,
  ) => void;
  onView: () => void;
  onRequestDelete: (img: ImageEntry) => void;
}

const MasonryImage: React.FC<MasonryImageProps> = ({
  img,
  selected,
  onSelectToggle,
  onDragStart,
  onView,
  onRequestDelete,
}) => {
  const [confirm, setConfirm] = useState(false);

  const handleImageClick = (
    e: ReactMouseEvent<HTMLImageElement> | ReactMouseEvent<HTMLVideoElement>,
  ): void => {
    const additive = e.metaKey || e.ctrlKey;
    if (additive) {
      e.preventDefault();
      onSelectToggle(img, true);
      return;
    }
    onView();
  };

  return (
    <motion.div
      data-img-url={img.url}
      initial={{ opacity: 0 }}
      animate={{ opacity: 1 }}
      exit={{ opacity: 0, y: -300, transition: { duration: 0.15 } }}
      className="group relative mb-4 break-inside-avoid select-none"
      draggable
      onDragStart={(e) => onDragStart(e, img)}
    >
      <div
        className={cn(
          "relative overflow-hidden rounded-md",
          selected && "ring-4 ring-blue-500",
        )}
      >
        {img.file.type.startsWith("video") ? (
          <video
            className="block w-full cursor-pointer select-none"
            onClick={handleImageClick}
            controls
            muted
            loop
            playsInline
          >
            <source src={img.url} type={img.file.type || "video/mp4"} />
            Your browser does not support the video tag.
          </video>
        ) : (
          <img
            src={img.url}
            alt={img.file.name}
            className="block w-full cursor-pointer select-none"
            onClick={handleImageClick}
          />
        )}
        <motion.button
          key={`delete-button-${img.url}`}
          whileHover={{ scale: 1.1 }}
          whileTap={{ scale: 0.9 }}
          transition={{ type: "spring", stiffness: 500, damping: 25 }}
          className="bg-background/70 absolute right-2 bottom-2 flex h-7 w-7 cursor-pointer items-center justify-center rounded-md opacity-0 backdrop-blur-sm transition-all duration-150 group-hover:opacity-100 hover:text-red-500"
          onClick={(e) => {
            e.stopPropagation();
            setConfirm(true);
          }}
        >
          <Trash2 className="h-4 w-4" />
        </motion.button>
      </div>

      <AnimatePresence>
        {confirm && (
          <motion.div
            key={`confirm-delete-${img.url}`}
            initial={{ opacity: 0 }}
            animate={{ opacity: 1 }}
            exit={{ opacity: 0 }}
            className="bg-background/70 absolute inset-0 z-10 flex flex-col items-center justify-center gap-2 rounded-md backdrop-blur-sm"
          >
            <div className="flex gap-2">
              <Button
                size="sm"
                onClick={() => onRequestDelete(img)}
                variant="destructive"
              >
                Delete
              </Button>
              <Button
                size="sm"
                variant="secondary"
                onClick={() => setConfirm(false)}
              >
                Cancel
              </Button>
            </div>
          </motion.div>
        )}
      </AnimatePresence>
    </motion.div>
  );
};

declare global {
  interface Window {
    showDirectoryPicker: () => Promise<FileSystemDirectoryHandle>;
  }

  interface FileSystemDirectoryHandle {
    entries(): AsyncIterable<[string, FileSystemHandle]>;
  }
}

const UNCATEGORIZED_KEY = "__UNSORTED__";

enum SortKey {
  DATE = "date",
  NAME = "name",
}

const useSupports = (supportCondition: string) => {
  const [checkResult, setCheckResult] = useState<boolean | undefined>();

  useEffect(() => {
    setCheckResult(CSS.supports(supportCondition));
  }, [supportCondition]);

  return checkResult;
};

export default function PhotoGallery(): React.ReactElement {
  const [rootDir, setRootDir] = useState<FileSystemDirectoryHandle | null>(
    null,
  );
  const [albums, setAlbums] = useState<Album[]>([]);
  const [photos, setPhotos] = useState<ImageEntry[]>([]);
  const [active, setActive] = useState<string>("");
  const [slider, setSlider] = useState<number>(3);
  const columns = 2 + 6 - slider;

  const [sortKey, setSortKey] = useState<SortKey>(SortKey.NAME);
  const [sortAsc, setSortAsc] = useState<boolean>(true);

  const [selection, setSelection] = useState<Set<ImageEntry>>(new Set());
  const dragRef = useRef<{ imgs: ImageEntry[] } | null>(null);
  const [highlightAlbumId, setHighlightAlbumId] = useState<string | null>(null);

  const [albumDialogOpen, setAlbumDialogOpen] = useState(false);
  const [newAlbumName, setNewAlbumName] = useState("");
  const [deleteAlbumConfirm, setDeleteAlbumConfirm] = useState(false);
  const [viewerIndex, setViewerIndex] = useState<number | null>(null);

  const [deleteConfirmOpen, setDeleteConfirmOpen] = useState(false);

  const [locked, setLocked] = useState<boolean>(false);

  const prevActiveIdx = useRef<number>(-1);

  const INITIAL_BATCH = 30;
  const BATCH_SIZE = 30;
  const [visibleCount, setVisibleCount] = useState<number>(INITIAL_BATCH);
  const sentinelRefs = useRef<HTMLDivElement[]>([]);
  const [albumsReady, setAlbumsReady] = useState<boolean>(false);
  const [albumLoadingStatus, setAlbumLoadingStatus] = useState<string>("");
  const refreshMutex = useRef<boolean>(false);

  const sortedAlbums = React.useMemo(() => {
    const sorted = [...albums].sort((a, b) => a.name.localeCompare(b.name));
    const uncategorized = sorted.find((a) => a.dirName === UNCATEGORIZED_KEY);
    if (uncategorized) {
      sorted.splice(sorted.indexOf(uncategorized), 1);
      sorted.unshift(uncategorized);
    }
    return sorted;
  }, [albums]);

  useEffect(() => {
    if (!photos) return;
    setSelection(
      (prev) => new Set(Array.from(prev).filter((img) => photos.includes(img))),
    );
  }, [photos]);

  const isImage = (n: string): boolean =>
    /\.(png|jpe?g|gif|bmp|webp|avif|mp4)$/i.test(n);

  const loadImageDims = (file: File): Promise<{ w: number; h: number }> =>
    new Promise((resolve, reject) => {
      const img = new Image();
      img.onload = () => resolve({ w: img.naturalWidth, h: img.naturalHeight });
      img.onerror = reject;
      img.src = URL.createObjectURL(file);
    });

  const convertAllHEIC = useCallback(
    async (dir: FileSystemDirectoryHandle): Promise<void> => {
      const heicFiles: string[] = [];
      for await (const [name, entry] of dir.entries()) {
        if (entry.kind !== "file") continue;
        const fileHandle = entry as FileSystemFileHandle;
        const file = await fileHandle.getFile();
        if (await isHeic(file)) {
          heicFiles.push(name);
        }
      }

      if (heicFiles.length === 0) return;
      setAlbumLoadingStatus("Converting HEIC files...");

      const meta = await MetadataManager.readDirMeta(dir);
      const albumName = meta.name ?? dir.name;

      let convertedCount = 0;
      const toastId = toast.loading(
        `Converting HEICs in "${albumName}" (${convertedCount}/${heicFiles.length})`,
        {
          duration: Infinity,
        },
      );

      try {
        for (const name of heicFiles) {
          const fileHandle = await dir.getFileHandle(name);
          const file = await fileHandle.getFile();

          try {
            const converted = await heicTo({
              blob: file,
              type: "image/png",
              quality: 0.7,
            });

            if (converted instanceof Blob) {
              const newFile = new File(
                [converted],
                name.split(".")[0] + ".png",
                {
                  type: "image/png",
                },
              );
              const newHandle = await dir.getFileHandle(newFile.name, {
                create: true,
              });
              const writable = await newHandle.createWritable();
              await writable.write(await newFile.arrayBuffer());
              await writable.close();
              await dir.removeEntry(name);
            } else {
              console.error(
                "HEIC conversion returned unexpected type:",
                converted,
              );
            }
          } catch (err) {
            console.error("Failed to convert HEIC file:", err);
          }

          convertedCount++;
          toast.loading(
            `Converting HEICs in "${albumName}" (${convertedCount}/${heicFiles.length})`,
            {
              id: toastId,
              duration: Infinity,
            },
          );
        }

        toast.success(
          `Converted ${convertedCount} HEIC file(s) in "${albumName}"`,
          {
            id: toastId,
            duration: 1000,
          },
        );
      } catch (error) {
        toast.error(`Failed to convert HEICs in "${albumName}"`, {
          id: toastId,
          duration: 3000,
        });
        console.error("HEIC conversion error:", error);
      }
    },
    [],
  );

  const loadAlbum = useCallback(
    async (dir: FileSystemDirectoryHandle, isRoot = false): Promise<Album> => {
      const meta = await MetadataManager.readDirMeta(dir);

      await convertAllHEIC(dir);

      const thumb = await (async (): Promise<string | null> => {
        for await (const [name, entry] of dir.entries()) {
          if (entry.kind === "file" && isImage(name)) {
            const fh = entry as FileSystemFileHandle;
            const file = await fh.getFile();
            const imeta = await MetadataManager.readFileMeta(dir, name);
            if (imeta.width && imeta.height) {
              return URL.createObjectURL(file);
            }
          }
        }
        return null;
      })();

      const images = await (async (): Promise<string[]> => {
        const names: string[] = [];
        for await (const [name, entry] of dir.entries()) {
          if (entry.kind === "file" && isImage(name)) {
            names.push(name);
          }
        }
        return names;
      })();

      return {
        dirName: isRoot ? "Uncategorized" : dir.name,
        name: isRoot ? "Uncategorized" : (meta.name ?? dir.name),
        meta,
        thumb,
        images,
        handle: dir,
      };
    },
    [convertAllHEIC],
  );

  const loadPhotos = useCallback(
    async (album: Album): Promise<ImageEntry[]> => {
      await Promise.all(
        album.images.map(async (name) => {
          const handle = await album.handle.getFileHandle(name);
          const file = await handle.getFile();
          const meta = await MetadataManager.readFileMeta(album.handle, name);
          if (!meta.width || !meta.height) {
            if (!file.type.startsWith("image/")) return;
            const { w, h } = await loadImageDims(file);
            meta.width = w;
            meta.height = h;
            MetadataManager.writeFileMeta(album.handle, name, meta).catch(
              () => undefined,
            );
          }
        }),
      );

      const entries: ImageEntry[] = [];
      for (const name of album.images) {
        const handle = await album.handle.getFileHandle(name);
        const file = await handle.getFile();
        const meta = await MetadataManager.readFileMeta(album.handle, name);
        if (!meta.added) {
          meta.added = Date.now();
          MetadataManager.writeFileMeta(album.handle, name, meta).catch(
            () => undefined,
          );
        }

        entries.push({
          file,
          url: URL.createObjectURL(file),
          meta,
          handle,
        });
      }
      return entries;
    },
    [],
  );

  const refreshAlbums = useCallback(
    async (root: FileSystemDirectoryHandle): Promise<void> => {
      if (refreshMutex.current) return;
      refreshMutex.current = true;
      try {
        if (!albumsReady && albumLoadingStatus !== "Loading albums...") {
          setAlbumLoadingStatus("Loading albums...");
        }
        const list: Album[] = [];
        for await (const [, entry] of root.entries())
          if (entry.kind === "directory")
            list.push(await loadAlbum(entry as FileSystemDirectoryHandle));
        list.unshift(await loadAlbum(root, true));
        list.forEach((album) => {
          const existing = albums.find((a) => a.dirName === album.dirName);
          if (!existing) {
            setAlbums((prev) => [...prev, album]);
            return;
          }

          if (
            !existing.images.some((img) => !album.images.includes(img)) &&
            !album.images.some((img) => !existing.images.includes(img))
          )
            return;

          loadPhotos(album)
            .then((newPhotos) => {
              if (active === album.dirName) {
                photos
                  .filter(
                    (p) => !newPhotos.some((n) => n.file.name === p.file.name),
                  )
                  .forEach((p) => URL.revokeObjectURL(p.url));

                setPhotos((prev) => {
                  return [
                    ...prev.filter((p) =>
                      newPhotos.some((n) => n.file.name === p.file.name),
                    ),
                    ...newPhotos.filter(
                      (n) => !prev.some((p) => p.file.name === n.file.name),
                    ),
                  ];
                });

                setAlbums((prev) =>
                  prev.map((a) =>
                    a.dirName === album.dirName
                      ? { ...a, images: newPhotos.map((n) => n.file.name) }
                      : a,
                  ),
                );
              }
            })
            .catch(console.error);
        });
        albums.forEach((album) => {
          if (!list.some((a) => a.dirName === album.dirName)) {
            setAlbums((prev) =>
              prev.filter((a) => a.dirName !== album.dirName),
            );
          }
        });

        if (!active && list.length && list[0]) {
          setActive(list[0].dirName);
          setPhotos(await loadPhotos(list[0]));
        }

        if (!albumsReady) {
          setAlbumsReady(true);
          setAlbumLoadingStatus("");
        }
      } finally {
        refreshMutex.current = false;
      }
    },
    [
      active,
      loadAlbum,
      setAlbums,
      setActive,
      albums,
      loadPhotos,
      photos,
      albumsReady,
      albumLoadingStatus,
    ],
  );

  useEffect(() => {
    if (!rootDir) return;
    const id = setInterval(() => void refreshAlbums(rootDir), 2000);
    return () => clearInterval(id);
  }, [rootDir, refreshAlbums]);

  const pickDirectory = async (): Promise<void> => {
    const dir = await window.showDirectoryPicker();
    setRootDir(dir);
    setAlbumsReady(false);
    setAlbumLoadingStatus("");
    setTimeout(() => {
      void refreshAlbums(dir);
    }, 1);
  };

  const createAlbum = async (): Promise<void> => {
    if (!rootDir || !newAlbumName.trim()) return;
    const safe = newAlbumName.trim().replace(/[\/\\:]/g, "_");
    const newDir = await rootDir.getDirectoryHandle(safe, { create: true });
    await MetadataManager.writeDirMeta(newDir, { name: newAlbumName.trim() });
    setAlbumDialogOpen(false);
    setNewAlbumName("");
    await refreshAlbums(rootDir);
  };

  const toggleSelect = (img: ImageEntry, additive: boolean): void => {
    setSelection((prev) => {
      const next = new Set(prev);
      if (additive) {
        if (next.has(img)) next.delete(img);
        else next.add(img);
      } else {
        next.clear();
        next.add(img);
      }
      return next;
    });
  };

  const clearSelection = (): void => setSelection(new Set());

  const createDragPreviewStack = (imgs: ImageEntry[]): HTMLDivElement => {
    const count = Math.min(imgs.length, 3);
    const container = document.createElement("div");
    container.style.position = "relative";
    container.style.width = "80px";
    container.style.height = "80px";
    container.style.userSelect = "none";
    container.style.pointerEvents = "none";
    container.style.borderRadius = "0.5rem";
    for (let i = 0; i < count; i += 1) {
      const imgEl = document.createElement("img");
      imgEl.src = imgs?.[i]?.url ?? "";
      imgEl.style.width = "80px";
      imgEl.style.height = "80px";
      imgEl.style.objectFit = "cover";
      imgEl.style.position = "absolute";
      imgEl.style.top = `${8 * i + 16}px`;
      imgEl.style.left = `${8 * i + 16}px`;
      imgEl.style.border = "1px solid rgba(0,0,0,0.25)";
      imgEl.style.borderRadius = "0.5rem";
      imgEl.draggable = false;
      container.appendChild(imgEl);
    }
    if (imgs.length > 3) {
      const overlay = document.createElement("div");
      overlay.textContent = `${imgs.length}`;
      overlay.style.position = "absolute";
      overlay.style.marginLeft = "32px";
      overlay.style.marginTop = "32px";
      overlay.style.width = "80px";
      overlay.style.height = "80px";
      overlay.style.display = "flex";
      overlay.style.alignItems = "center";
      overlay.style.justifyContent = "center";
      overlay.style.backdropFilter = "blur(4px)";
      overlay.style.background = "rgba(0,0,0,0.4)";
      overlay.style.color = "white";
      overlay.style.fontWeight = "bold";
      overlay.style.fontSize = "20px";
      overlay.style.borderRadius = "0.5rem";
      container.appendChild(overlay);
    }
    return container;
  };

  const animateDragFly = (
    imgs: ImageEntry[],
    rects: (DOMRect | undefined)[],
    x: number,
    y: number,
  ): void => {
    const flyers: HTMLImageElement[] = [];
    imgs.forEach((img, i) => {
      const rect = rects[i];
      if (!rect) return;
      const flyer = document.createElement("img");
      flyer.src = img.url;
      flyer.style.position = "fixed";
      flyer.style.top = `${rect.top}px`;
      flyer.style.left = `${rect.left}px`;
      flyer.style.width = `${rect.width}px`;
      flyer.style.height = `${rect.height}px`;
      flyer.style.objectFit = "cover";
      flyer.style.borderRadius = "1rem";
      flyer.style.zIndex = "9999";
      flyer.style.pointerEvents = "none";
      flyer.style.userSelect = "none";
      flyer.style.transition = "transform 0.2s linear, opacity 0.2s ease";
      flyer.style.transform = `scale(1)`;
      flyer.style.opacity = "1";
      document.body.appendChild(flyer);
      flyers.push(flyer);
    });

    let targetX = x;
    let targetY = y;
    const startTime = Date.now();
    let lastUpdate: number | null = null;

    const update = (): void => {
      if (lastUpdate && Date.now() - lastUpdate < 100) return;
      lastUpdate = Date.now();
      flyers.forEach((flyer, i) => {
        const r = rects[i];
        if (!r) return;
        flyer.style.transition = `transform ${Math.max(0.1, 0.2 - (Date.now() - startTime) / 1000)}s linear, opacity 0.2s ease`;
        flyer.style.transform = `translate(${targetX - r.left - r.width / 2}px, ${targetY - r.top - r.height / 2}px) scale(0.2)`;
      });
    };

    requestAnimationFrame(() => {
      requestAnimationFrame(() => {
        update();
      });
    });

    const movePointer = (e: PointerEvent): void => {
      targetX = e.clientX;
      targetY = e.clientY;
      update();
    };

    const moveDrag = (e: DragEvent): void => {
      targetX = e.clientX;
      targetY = e.clientY;
      update();
    };

    window.addEventListener("pointermove", movePointer);
    window.addEventListener("dragover", moveDrag);

    const fadeTimeout = window.setTimeout(() => {
      flyers.forEach((f) => (f.style.opacity = "0"));
    }, 200);

    window.setTimeout(() => {
      flyers.forEach((f) => f.remove());
      window.removeEventListener("pointermove", movePointer);
      window.removeEventListener("dragover", moveDrag);
      window.clearTimeout(fadeTimeout);
    }, 400);
  };

  const onDragStartInternal = (
    e: MouseEvent | TouchEvent | PointerEvent | ReactDragEvent,
    img: ImageEntry,
  ): void => {
    if (
      e instanceof PointerEvent ||
      e instanceof MouseEvent ||
      e instanceof TouchEvent
    )
      return;
    const imgs = selection.has(img) ? Array.from(selection) : [img];
    dragRef.current = { imgs };

    const stackPreview = createDragPreviewStack(imgs);
    stackPreview.style.position = "absolute";
    stackPreview.style.top = "-9999px";
    stackPreview.style.left = "-9999px";
    document.body.appendChild(stackPreview);
    const rectPreview = stackPreview.getBoundingClientRect();
    e.dataTransfer.setDragImage(
      stackPreview,
      rectPreview.width / 2,
      rectPreview.height / 2,
    );
    requestAnimationFrame(() => document.body.removeChild(stackPreview));
    e.dataTransfer.effectAllowed = "move";

    const rects = imgs.map((i) => {
      const el = document.querySelector(
        `[data-img-url="${CSS.escape(i.url)}"]`,
      );
      return el?.getBoundingClientRect();
    });

    animateDragFly(imgs, rects, e.clientX, e.clientY);
  };

  const moveImagesToAlbum = async (
    target: Album,
    imgs: ImageEntry[],
  ): Promise<void> => {
    const discarded: ImageEntry[] = [];
    const totalCount = imgs.length;
    let processedCount = 0;

    const toastId = toast.loading(
      `Moving images to "${target.name}" (${processedCount}/${totalCount})`,
      {
        duration: Infinity,
      },
    );

    for (const img of imgs) {
      if (target.images.some((i) => i === img.file.name)) {
        discarded.push(img);
        processedCount++;
        toast.loading(
          `Moving images to "${target.name}" (${processedCount}/${totalCount})`,
          {
            id: toastId,
            duration: Infinity,
          },
        );
        continue;
      }
      const srcFile = await img.handle.getFile();
      const targetHandle = await target.handle.getFileHandle(img.file.name, {
        create: true,
      });
      const writable = await targetHandle.createWritable();
      await writable.write(await srcFile.arrayBuffer());
      await writable.close();
      const parent = albums.find((a) => a.images.includes(img.file.name));
      if (parent) {
        await parent.handle.removeEntry(img.file.name);
        parent.images = parent.images.filter((i) => i !== img.file.name);
      }
      target.images.push(img.file.name);

      processedCount++;
      toast.loading(
        `Moving images to "${target.name}" (${processedCount}/${totalCount})`,
        {
          id: toastId,
          duration: Infinity,
        },
      );
    }

    photos
      .filter(
        (p) =>
          imgs.some((i) => i.file.name === p.file.name) &&
          !discarded.some((i) => i.file.name === p.file.name),
      )
      .forEach((p) => URL.revokeObjectURL(p.url));
    setPhotos((prev) =>
      prev.map((p) => {
        if (
          discarded.some((i) => i.file.name === p.file.name) ||
          !imgs.some((i) => i.file.name === p.file.name)
        ) {
          return p;
        }
        return {
          ...p,
          deleted: true,
        };
      }),
    );
    setSelection(new Set());

    if (discarded.length) {
      toast.error(
        `Skipped ${discarded.length} image(s) already in album "${target.name}"`,
        {
          id: toastId,
          duration: 3000,
        },
      );
    } else {
      toast.success(
        `Moved ${imgs.length - discarded.length} image(s) to album "${target.name}"`,
        {
          id: toastId,
          duration: 2000,
        },
      );
    }
  };

  const onAlbumInternalDrop = async (album: Album): Promise<void> => {
    const dragged = dragRef.current?.imgs;
    dragRef.current = null;
    setHighlightAlbumId(null);
    if (!dragged?.length) return;
    await moveImagesToAlbum(album, dragged);
  };

  const onAlbumExternalDrop = useCallback(
    async (album: Album, files: FileList): Promise<void> => {
      const imageFiles = Array.from(files).filter((file) => isImage(file.name));
      if (imageFiles.length === 0) return;

      let processedCount = 0;
      const totalCount = imageFiles.length;

      const toastId = toast.loading(
        `Adding images to "${album.name}" (${processedCount}/${totalCount})`,
        {
          duration: Infinity,
        },
      );

      const tasks: Promise<void>[] = [];
      imageFiles.forEach((file) => {
        tasks.push(
          (async (): Promise<void> => {
            if (album.images.some((i) => i === file.name)) {
              processedCount++;
              toast.loading(
                `Adding images to "${album.name}" (${processedCount}/${totalCount})`,
                {
                  id: toastId,
                  duration: Infinity,
                },
              );
              return;
            }
            const fh = await album.handle.getFileHandle(file.name, {
              create: true,
            });
            const w = await fh.createWritable();
            await w.write(await file.arrayBuffer());
            await w.close();
            const entry: ImageEntry = {
              file,
              url: URL.createObjectURL(file),
              meta: { added: Date.now() },
              handle: fh,
            };
            MetadataManager.writeFileMeta(
              album.handle,
              file.name,
              entry.meta,
            ).catch(() => undefined);
            album.images.push(file.name);
            if (active === album.dirName) {
              setPhotos((prev) => [...prev, entry]);
            }
            processedCount++;
            toast.loading(
              `Adding images to "${album.name}" (${processedCount}/${totalCount})`,
              {
                id: toastId,
                duration: Infinity,
              },
            );
          })(),
        );
      });

      await Promise.all(tasks);

      toast.success(
        `Added ${imageFiles.length} image(s) to album "${album.name}"`,
        {
          id: toastId,
          duration: 2000,
        },
      );
    },
    [active],
  );

  const onGridExternalDrop = async (e: ReactDragEvent): Promise<void> => {
    e.preventDefault();
    if (!e.dataTransfer.files.length || !active) return;
    const album = albums.find((a) => a.dirName === active);
    if (album) await onAlbumExternalDrop(album, e.dataTransfer.files);
  };

  const onAlbumDragOver = (albumId: string): void | null =>
    dragRef.current && setHighlightAlbumId(albumId);
  const clearAlbumHighlight = (): void => setHighlightAlbumId(null);

  const deletePhotos = async (imgs: ImageEntry[]): Promise<void> => {
    if (!activeAlbum) return;
    for (const img of imgs) {
      const parent = albums.find(
        (a) =>
          a.images.includes(img.file.name) && a.dirName === activeAlbum.dirName,
      );
      if (!parent) continue;
      await parent.handle.removeEntry(img.file.name);
      try {
        const base = img.file.name.replace(/\.[^.]+$/, "");
        await parent.handle.removeEntry(`.${base}.json`);
      } catch {}
      parent.images = parent.images.filter((i) => i !== img.file.name);
      setPhotos((prev) => {
        return prev.map((p) => {
          if (p.file.name === img.file.name) {
            URL.revokeObjectURL(p.url);
            return { ...p, deleted: true };
          }
          return p;
        });
      });
    }
    setSelection(new Set());
    setDeleteConfirmOpen(false);
    toast(`Deleted ${imgs.length} image(s)`, {
      icon: "success",
      duration: 2000,
    });
  };

  const deleteAlbum = async (album: Album): Promise<void> => {
    if (album.dirName === UNCATEGORIZED_KEY || !rootDir) return;
    await rootDir.removeEntry(album.dirName, { recursive: true });
    setDeleteAlbumConfirm(false);
    await refreshAlbums(rootDir);
    const uncategorized = albums.find((a) => a.dirName === UNCATEGORIZED_KEY);
    if (uncategorized) {
      setActive(UNCATEGORIZED_KEY);
      setPhotos(await loadPhotos(uncategorized));
    }
    toast(`Album ${album.name} deleted`, {
      icon: "success",
      duration: 2000,
    });
  };

  const activeAlbum = albums.find((a) => a.dirName === active);

  useEffect(() => {
    setVisibleCount(INITIAL_BATCH);
  }, [active]);

  const sortedFilteredImgs: ImageEntry[] = React.useMemo(() => {
    if (!photos) return [];
    const cmp: Record<SortKey, (a: ImageEntry, b: ImageEntry) => number> = {
      [SortKey.DATE]: (a, b) => (a.meta.added ?? 0) - (b.meta.added ?? 0),
      [SortKey.NAME]: (a, b) => a.file.name.localeCompare(b.file.name),
    };
    const sorted = [...photos].sort(cmp[sortKey]);
    return sortAsc ? sorted : sorted.reverse();
  }, [sortKey, sortAsc, photos]);

  const imagesInColumns: ImageEntry[][] = React.useMemo(() => {
    type Col = { h: number; imgs: ImageEntry[] };
    const cols: Col[] = Array.from({ length: columns }, () => ({
      h: 0,
      imgs: [],
    }));
    sortedFilteredImgs.forEach((img) => {
      const target = cols.reduce((a, b) => (a.h <= b.h ? a : b));
      const ratio = (img.meta.height ?? 1) / (img.meta.width ?? 1);
      target.imgs.push(img);
      target.h += ratio;
    });
    return cols.map((c) => c.imgs);
  }, [sortedFilteredImgs, columns]);

  useEffect(() => {
    const handler = (ev: KeyboardEvent): void => {
      if (document.activeElement instanceof HTMLInputElement) return;
      const key = ev.key.toLowerCase();
      if (locked) {
        if (key === "u" || key === "г") setLocked(false);
        return;
      }
      if (key === "l" || key === "д") {
        setLocked(true);
        clearSelection();
        setViewerIndex(null);
        return;
      }
      if (viewerIndex !== null) {
        if (ev.key === "ArrowLeft")
          setViewerIndex((i) =>
            i === null
              ? null
              : (i - 1 + sortedFilteredImgs.length) % sortedFilteredImgs.length,
          );
        if (ev.key === "ArrowRight")
          setViewerIndex((i) =>
            i === null ? null : (i + 1) % sortedFilteredImgs.length,
          );
        if (ev.key === "Escape") setViewerIndex(null);
        return;
      }
      if ((ev.metaKey || ev.ctrlKey) && (key === "a" || key === "ф")) {
        ev.preventDefault();
        setSelection(new Set(sortedFilteredImgs));
        return;
      }
      if ((ev.metaKey || ev.ctrlKey) && (key === "d" || key === "в")) {
        ev.preventDefault();
        setSelection(new Set());
        return;
      }
      const deletePressed =
        ev.key === "Delete" || (ev.key === "Backspace" && ev.metaKey);
      if (deletePressed && selection.size) {
        ev.preventDefault();
        setDeleteConfirmOpen(true);
      }
    };
    document.addEventListener("keydown", handler);
    return (): void => document.removeEventListener("keydown", handler);
  }, [selection, viewerIndex, sortedFilteredImgs, locked]);

  useEffect(() => {
    const idx = albums.findIndex((a) => a.dirName === active);
    prevActiveIdx.current = idx;
  }, [active, albums]);

  useEffect(() => {
    if (!active) return;

    const handlePaste = (e: ClipboardEvent) => {
      const imgs: File[] = [];
      for (const item of e.clipboardData?.items ?? []) {
        if (item.kind === "file" && item.type.startsWith("image/")) {
          const f = item.getAsFile();
          if (f) imgs.push(f);
        }
      }
      if (!imgs.length) return;

      e.preventDefault();
      void onAlbumExternalDrop(
        albums.find((a) => a.dirName === active)!,
        imgs as unknown as FileList,
      );
    };

    window.addEventListener("paste", handlePaste);
    return () => window.removeEventListener("paste", handlePaste);
  }, [albums, active, onAlbumExternalDrop]);

  useEffect(() => {
    if (!sentinelRefs.current) return;

    console.log("Updating sentinels", sentinelRefs.current);

    const observer = new IntersectionObserver(
      ([entry]) => {
        if (!entry?.isIntersecting) return;
        setVisibleCount((c) =>
          Math.min(c + BATCH_SIZE, sortedFilteredImgs.length),
        );
      },
      {
        root: null,
        rootMargin: `${window.innerHeight * 5}px`,
        threshold: 0.1,
      },
    );

    sentinelRefs.current.forEach((el) => observer.observe(el));
    return () => observer.disconnect();
  }, [sortedFilteredImgs.length, columns]);

  const isFirefox = useSupports("-moz-appearance:none");
  if (isFirefox) {
    return (
      <div className="bg-background flex min-h-screen items-center justify-center pb-6">
        <div className="text-center">
          <CircleX className="text-muted-foreground mx-auto mb-4 h-12 w-12" />
          <h1 className="mb-4 text-2xl font-black">Unsupported Browser</h1>
          <p className="text-muted-foreground text-lg">
            This application does not support Firefox due to technical
            limitations.
          </p>
        </div>
      </div>
    );
  }

  return (
    <div
      className="relative flex min-h-screen select-none"
      onDrop={onGridExternalDrop}
      onDragOver={(e) => e.preventDefault()}
    >
      <Toaster theme="dark" />
      <div className="border-border bg-background/95 w-64 space-y-3 border-r p-4">
        {!rootDir && (
          <Button onClick={pickDirectory} className="w-full">
            <FolderOpen className="h-4 w-4" /> Choose Directory
          </Button>
        )}
        <Popover open={albumDialogOpen} onOpenChange={setAlbumDialogOpen}>
          <PopoverTrigger asChild>
            <Button
              variant="secondary"
              className="w-full"
              disabled={!rootDir}
              onClick={() => setAlbumDialogOpen(true)}
            >
              <Plus className="h-4 w-4" /> New Album
            </Button>
          </PopoverTrigger>
          <PopoverContent className="ml-8 w-64 bg-black/60 backdrop-blur-2xl">
            <p className="mb-2 text-sm">Enter a name for the new album</p>
            <div className="flex flex-col gap-2">
              <Input
                value={newAlbumName}
                onChange={(e) => setNewAlbumName(e.target.value)}
                placeholder="Album name"
              />
              <Button onClick={createAlbum}>
                <Plus className="h-4 w-4" />
                Create
              </Button>
            </div>
          </PopoverContent>
        </Popover>
        {rootDir && !albumsReady && (
          <div className="text-muted-foreground flex w-full items-center justify-center gap-2 text-sm">
            <Loader2 className="text-muted-foreground h-6 w-6 animate-spin" />
            {albumLoadingStatus}
          </div>
        )}
        <ScrollArea
          className="h-[calc(100vh-6rem)] p-2"
          onDragLeave={clearAlbumHighlight}
        >
          <AnimatePresence>
            {sortedAlbums.map((album) => (
              <AlbumItem
                key={album.dirName}
                album={album}
                active={album.dirName === active}
                highlighted={highlightAlbumId === album.dirName}
                onClick={async () => {
                  if (photos) {
                    photos.forEach((p) => URL.revokeObjectURL(p.url));
                  }
                  setPhotos(await loadPhotos(album));
                  setActive(album.dirName);
                }}
                onDropExternal={(files) => onAlbumExternalDrop(album, files)}
                onDropInternal={() => onAlbumInternalDrop(album)}
                onDragOver={() => onAlbumDragOver(album.dirName)}
              />
            ))}
          </AnimatePresence>
        </ScrollArea>
      </div>

      <div className="flex h-screen flex-1 flex-col">
        <div className="border-border bg-background/95 relative flex flex-wrap items-center gap-4 border-b px-4 py-2">
          {active ? (
            <>
              <span className="text-sm font-medium">Zoom</span>
              <Slider
                value={[slider]}
                min={0}
                max={6}
                step={1}
                className="w-40"
                onValueChange={([v]): void => (v && setSlider(v)) as void}
              />
            </>
          ) : null}
          <Select
            value={sortKey}
            onValueChange={(v) => setSortKey(v as SortKey)}
          >
            <SelectTrigger className="w-36">
              <SelectValue />
            </SelectTrigger>
            <SelectContent>
              <SelectItem value={SortKey.DATE}>Date added</SelectItem>
              <SelectItem value={SortKey.NAME}>Filename</SelectItem>
            </SelectContent>
          </Select>
          <Toggle
            pressed={!sortAsc}
            onPressedChange={setSortAsc}
            aria-label="toggle sort dir"
          >
            {!sortAsc ? (
              <ArrowUpAZ className="h-4 w-4" />
            ) : (
              <ArrowDownAZ className="h-4 w-4" />
            )}
          </Toggle>
          {active && active !== UNCATEGORIZED_KEY && (
            <Popover
              open={deleteAlbumConfirm}
              onOpenChange={setDeleteAlbumConfirm}
            >
              <PopoverTrigger asChild>
                <Button
                  variant="ghost"
                  size="sm"
                  className="ml-auto text-red-500 hover:text-red-500"
                >
                  <Trash2 className="h-4 w-4" /> Delete album
                </Button>
              </PopoverTrigger>
              <PopoverContent className="mr-8 w-64 bg-black/60 backdrop-blur-2xl">
                <p className="mb-2 text-sm">
                  Delete this album and all its photos?
                </p>
                <Button
                  variant="destructive"
                  className="mb-2 w-full"
                  onClick={() => deleteAlbum(activeAlbum!)}
                >
                  Delete
                </Button>
                <Button
                  variant="secondary"
                  className="w-full"
                  onClick={() => setDeleteAlbumConfirm(false)}
                >
                  Cancel
                </Button>
              </PopoverContent>
            </Popover>
          )}
        </div>

        <AnimatePresence>
          {selection.size > 1 && (
            <motion.div
              key="multi-action-bar"
              initial={{ opacity: 0, y: -10 }}
              animate={{ opacity: 1, y: 0 }}
              exit={{ opacity: 0, y: -10 }}
              className="bg-background/70 fixed top-4 right-4 z-50 flex flex-col gap-2 rounded-lg p-4 backdrop-blur-lg"
            >
              <Popover
                open={deleteConfirmOpen}
                onOpenChange={setDeleteConfirmOpen}
              >
                <PopoverTrigger asChild>
                  <Button
                    variant="ghost"
                    className="text-destructive hover:bg-destructive/20 justify-start"
                    onClick={() => setDeleteConfirmOpen(true)}
                  >
                    <Trash2 className="h-4 w-4" /> Delete ({selection.size})
                  </Button>
                </PopoverTrigger>
                <PopoverContent className="mr-8 w-64 bg-black/60 backdrop-blur-2xl">
                  <p className="mb-2 text-sm">
                    Delete {selection.size} selected photos?
                  </p>
                  <Button
                    variant="destructive"
                    className="mb-2 w-full"
                    onClick={() => {
                      deletePhotos(Array.from(selection)).catch(
                        () => undefined,
                      );
                      setDeleteConfirmOpen(false);
                    }}
                  >
                    Delete ({selection.size})
                  </Button>
                  <Button
                    variant="secondary"
                    className="w-full"
                    onClick={() => setDeleteConfirmOpen(false)}
                  >
                    Cancel
                  </Button>
                </PopoverContent>
              </Popover>
              <Popover>
                <PopoverTrigger asChild>
                  <Button
                    variant="ghost"
                    className="hover:bg-accent/20 justify-start"
                  >
                    <SendToBack className="h-4 w-4" /> Move to
                  </Button>
                </PopoverTrigger>
                <PopoverContent className="max-h-64 w-48 space-y-1 overflow-y-auto bg-black/60 backdrop-blur-2xl">
                  {albums
                    .filter((a) => a.dirName !== active)
                    .map((a) => (
                      <Button
                        key={a.dirName}
                        variant="ghost"
                        className="w-full justify-start hover:bg-black/15"
                        onClick={() =>
                          moveImagesToAlbum(a, Array.from(selection))
                        }
                      >
                        {a.name}
                      </Button>
                    ))}
                </PopoverContent>
              </Popover>
              <Button
                variant="ghost"
                className="hover:bg-accent/20 justify-start"
                onClick={clearSelection}
              >
                <X className="h-4 w-4" /> Clear selection
              </Button>
            </motion.div>
          )}
        </AnimatePresence>

        <div
          className="flex-1 overflow-y-auto p-4"
          onDragOver={(e) => e.preventDefault()}
        >
          {!sortedFilteredImgs.length && active && (
            <div className="text-muted-foreground w-full text-center text-lg font-medium">
              No photos found
            </div>
          )}
          <AnimatePresence>
            {sortedFilteredImgs.length > 0 && (
              <div
                key={active}
                className="grid gap-4"
                style={{
                  gridTemplateColumns: `repeat(${columns}, 1fr)`,
                }}
              >
                {Array.from({ length: columns }).map((_, colIndex) => (
                  <div key={`col-${colIndex}`} className="flex flex-col">
                    {imagesInColumns[colIndex]!.slice(
                      0,
                      Math.ceil(visibleCount / columns),
                    ).map((img) =>
                      img.deleted ? (
                        <div key={img.url} />
                      ) : (
                        <MasonryImage
                          key={img.url}
                          img={img}
                          selected={selection.has(img)}
                          onSelectToggle={toggleSelect}
                          onDragStart={onDragStartInternal}
                          onView={() =>
                            setViewerIndex(sortedFilteredImgs.indexOf(img))
                          }
                          onRequestDelete={(i) => deletePhotos([i])}
                        />
                      ),
                    )}
                    <div
                      ref={(el) => {
                        if (el) sentinelRefs.current[colIndex] = el;
                      }}
                      style={{ height: 1 }}
                    />
                  </div>
                ))}
              </div>
            )}
          </AnimatePresence>
        </div>
      </div>

      <AnimatePresence>
        {viewerIndex !== null && (
          <motion.div
            key="viewer"
            initial={{ opacity: 0 }}
            animate={{ opacity: 1 }}
            exit={{ opacity: 0 }}
            className="fixed inset-0 z-50 flex items-center justify-center bg-black/90"
            onClick={() => setViewerIndex(null)}
          >
            {sortedFilteredImgs?.[viewerIndex]?.file.type.startsWith(
              "video",
            ) ? (
              <video
                className="block w-full cursor-pointer select-none"
                controls
                muted
                loop
                playsInline
              >
                <source
                  src={sortedFilteredImgs?.[viewerIndex]?.url}
                  type={
                    sortedFilteredImgs?.[viewerIndex]?.file.type || "video/mp4"
                  }
                />
                Your browser does not support the video tag.
              </video>
            ) : (
              <img
                src={sortedFilteredImgs?.[viewerIndex]?.url}
                alt={sortedFilteredImgs?.[viewerIndex]?.file.name}
                className="block w-full cursor-pointer select-none"
              />
            )}

            <Button
              variant="ghost"
              size="icon"
              onClick={(e) => {
                e.stopPropagation();
                setViewerIndex(
                  (viewerIndex - 1 + sortedFilteredImgs.length) %
                    sortedFilteredImgs.length,
                );
              }}
              className="absolute top-1/2 left-4 -translate-y-1/2"
            >
              <ArrowLeft className="h-8 w-8" />
            </Button>
            <Button
              variant="ghost"
              size="icon"
              onClick={(e) => {
                e.stopPropagation();
                setViewerIndex((viewerIndex + 1) % sortedFilteredImgs.length);
              }}
              className="absolute top-1/2 right-4 -translate-y-1/2"
            >
              <ArrowRight className="h-8 w-8" />
            </Button>
          </motion.div>
        )}
      </AnimatePresence>

      <AnimatePresence>
        {locked && (
          <motion.div
            key="lockscreen"
            initial={{ opacity: 0 }}
            animate={{ opacity: 1, transition: { duration: 0.05 } }}
            exit={{ opacity: 0, transition: { duration: 0.3 } }}
            className="fixed inset-0 z-[9999] flex flex-col items-center justify-center gap-4 bg-black/70 pb-6 backdrop-blur-2xl"
          >
            <EyeOff className="h-14 w-14" />
            <span className="text-2xl font-medium">Sensitive content</span>
            <span className="text-muted-foreground max-w-sm text-center text-base">
              This website contains sensitive content which some people may find
              offensive or disturbing.
            </span>
          </motion.div>
        )}
      </AnimatePresence>
    </div>
  );
}
