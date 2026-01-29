/* eslint-disable @next/next/no-img-element */
"use client";

import { FAVORITES_ALBUM_ID } from "@/lib/consts";
import { markNonDuplicates } from "@/lib/fs/albumService";
import { useUpload } from "@/lib/hooks/use-upload";
import { useRoom237 } from "@/lib/stores";
import type { MediaEntry } from "@/lib/types";
import { AnimatePresence, motion } from "framer-motion";
import { IconTrash, IconX } from "@tabler/icons-react";
import {
  memo,
  startTransition,
  useCallback,
  useEffect,
  useRef,
  useState,
  useMemo,
} from "react";
import { useStoreWithEqualityFn } from "zustand/traditional";
import { Button } from "./ui/button";
import { useI18n } from "@/lib/i18n";

function ScrollRightFade({
  children,
  className,
}: {
  children: React.ReactNode;
  className: string;
}) {
  const ref = useRef<HTMLDivElement | null>(null);
  const [showFade, setShowFade] = useState(false);

  const update = useCallback(() => {
    const el = ref.current;
    if (!el) return;
    const max = el.scrollWidth - el.clientWidth;
    const atEnd = el.scrollLeft >= max - 32;
    setShowFade(max > 1 && !atEnd);
  }, []);

  useEffect(() => {
    const el = ref.current;
    if (!el) return;

    update();

    const onScroll = () => update();
    el.addEventListener("scroll", onScroll, { passive: true });

    const ro = new ResizeObserver(() => update());
    ro.observe(el);

    return () => {
      el.removeEventListener("scroll", onScroll);
      ro.disconnect();
    };
  }, [update]);

  return (
    <div className="relative">
      <div ref={ref} className={className}>
        {children}
      </div>
      <AnimatePresence>
        {showFade && (
          <motion.div
            initial={{ opacity: 0 }}
            animate={{ opacity: 1 }}
            exit={{ opacity: 0 }}
            className="pointer-events-none absolute top-0 right-0 bottom-2 left-auto w-12 bg-linear-to-r from-transparent to-black/50"
          />
        )}
      </AnimatePresence>
    </div>
  );
}

const duplicateEqual = (
  a: { image: MediaEntry; onDelete: () => void },
  b: { image: MediaEntry; onDelete: () => void },
) => a.image === b.image && a.onDelete === b.onDelete;

const Duplicate = memo(function Duplicate({
  image,
  onDelete,
}: {
  image: MediaEntry;
  onDelete: () => void;
}) {
  const [confirm, setConfirm] = useState(false);

  return (
    <div className="border-border group relative aspect-square overflow-hidden rounded-xl border">
      <div className="text-foreground bg-background/70 absolute top-2 left-2 rounded-xl px-2 py-0.5 text-xs opacity-0 backdrop-blur-lg transition-all duration-150 group-hover:opacity-100">
        {image.name}
      </div>
      <AnimatePresence>
        {confirm && (
          <motion.div
            initial={{ opacity: 0 }}
            animate={{ opacity: 1 }}
            exit={{ opacity: 0 }}
            className="bg-background/70 absolute inset-0 z-10 flex scale-105 flex-col items-center justify-center gap-2 backdrop-blur-sm"
          >
            <div className="flex gap-1">
              <Button
                size="sm"
                onClick={() => onDelete()}
                variant="destructive"
              >
                <IconTrash className="text-red-500" />
              </Button>
              <Button
                size="sm"
                variant="outline"
                onClick={() => setConfirm(false)}
              >
                <IconX />
              </Button>
            </div>
          </motion.div>
        )}
      </AnimatePresence>
      <motion.button
        whileHover={{ scale: 1.1 }}
        whileTap={{ scale: 0.9 }}
        transition={{ type: "spring", stiffness: 500, damping: 25 }}
        className="bg-background/70 absolute right-2 bottom-2 flex h-7 w-7 cursor-pointer items-center justify-center rounded-md opacity-0 backdrop-blur-sm transition-opacity duration-150 group-hover:opacity-100 hover:text-red-500"
        onClick={(e) => {
          e.stopPropagation();
          setConfirm(true);
        }}
        onPointerDownCapture={(e) => e.stopPropagation()}
      >
        <IconTrash className="h-4 w-4" />
      </motion.button>
      <img
        src={image.thumb}
        alt={image.name}
        className="h-full w-full rounded-xl object-cover"
      />
    </div>
  );
}, duplicateEqual);

export function DuplicatesView() {
  const [ready, setReady] = useState(false);
  const [duplicates, setDuplicates] = useState<string[][]>([]);
  const { t } = useI18n();
  const albumSlice = useStoreWithEqualityFn(
    useRoom237,
    (state) => {
      const id = state.activeAlbumId;
      if (!id || id === FAVORITES_ALBUM_ID) return null;
      const album = state.albumsById[id];
      if (!album) return null;
      return {
        id: album.albumId,
        path: album.path,
        medias: state.albumMediasByPath[album.path] ?? null,
      };
    },
    (a, b) =>
      a?.id === b?.id &&
      a?.path === b?.path &&
      (a?.medias?.length ?? -1) === (b?.medias?.length ?? -1),
  );
  const { deleteMedia } = useUpload();
  const showDuplicates = useRoom237((state) => state.showDuplicates);
  const duplicatesAvailable = useRoom237((state) => state.duplicatesAvailable);
  const setDuplicatesAvailable = useRoom237(
    (state) => state.setDuplicatesAvailable,
  );
  const setDuplicatesLoading = useRoom237(
    (state) => state.setDuplicatesLoading,
  );
  const batchOperationInProgress = useRoom237(
    (state) => state.batchOperationInProgress,
  );

  const mediaByName = useMemo(() => {
    const map = new Map<string, MediaEntry>();
    (albumSlice?.medias ?? []).forEach((m) => map.set(m.name, m));
    return map;
  }, [albumSlice?.medias]);

  const normalize = useCallback(
    (groups: string[][]) =>
      (groups ?? [])
        .map((group) => group.filter(Boolean))
        .filter((group) => group.length > 1),
    [],
  );

  useEffect(() => {
    const hasAny = duplicates.some((group) => group.length > 1);
    setDuplicatesAvailable(hasAny);
  }, [duplicates, setDuplicatesAvailable]);

  const refreshDuplicates = useCallback(
    async (options?: { initial?: boolean; force?: boolean }) => {
      const state = useRoom237.getState();
      if (albumSlice?.id !== state.activeAlbumId) {
        return;
      }

      if (options?.initial) {
        setReady(false);
      }
      setDuplicatesLoading(true);

      try {
        const latest = await state.loadAlbumDuplicates(albumSlice.id, {
          force: options?.force,
        });
        const normalized = normalize(latest);
        startTransition(() =>
          setDuplicates((prev) => {
            const prevKey = prev.map((g) => g.join("|")).join("||");
            const nextKey = normalized.map((g) => g.join("|")).join("||");
            if (prevKey === nextKey) return prev;
            return normalized;
          }),
        );
      } catch (error) {
        console.error("Failed to load duplicates", error);
        setDuplicates((prev) => prev);
      } finally {
        if (options?.initial !== false) {
          setReady(true);
        }
        setDuplicatesLoading(false);
      }
    },
    [albumSlice, normalize, setDuplicatesLoading],
  );

  useEffect(() => {
    if (!albumSlice) {
      setDuplicates([]);
      return;
    }
    const state = useRoom237.getState();
    const latest = state.albumDuplicatesByPath[albumSlice.path] ?? [];
    setDuplicates(normalize(latest));
  }, [albumSlice, normalize]);

  useEffect(() => {
    let cancelled = false;
    void (async () => {
      await refreshDuplicates({ initial: true, force: true });
      if (cancelled) return;
    })();

    return () => {
      cancelled = true;
    };
  }, [refreshDuplicates]);

  const prevAlbumRef = useRef<{ path?: string; count: number }>({
    path: undefined,
    count: 0,
  });

  useEffect(() => {
    const count = albumSlice?.medias?.length ?? 0;
    const prev = prevAlbumRef.current;

    if (!albumSlice) {
      prevAlbumRef.current = { path: undefined, count: 0 };
      return;
    }

    if (albumSlice.path !== prev.path) {
      prevAlbumRef.current = { path: albumSlice.path, count };
      return;
    }

    if (count > prev.count) {
      prevAlbumRef.current = { path: albumSlice.path, count };
      if (!batchOperationInProgress) {
        void refreshDuplicates({ initial: false, force: true });
      }
    } else {
      prevAlbumRef.current = { path: albumSlice.path, count };
    }
  }, [
    albumSlice,
    albumSlice?.medias?.length,
    refreshDuplicates,
    batchOperationInProgress,
  ]);

  useEffect(() => {
    if (!batchOperationInProgress && albumSlice) {
      void refreshDuplicates({ initial: false, force: true });
    }
  }, [batchOperationInProgress, albumSlice, refreshDuplicates]);

  if (!albumSlice) {
    return null;
  }

  return (
    <AnimatePresence>
      {showDuplicates && duplicatesAvailable && (
        <motion.div
          key="duplicates-viewer"
          className="border-border bg-background/60 fixed right-6 w-[60vw] origin-top overflow-y-scroll rounded-2xl border backdrop-blur-lg"
          style={{
            maxHeight: `calc(100vh - 4.5rem)`,
            top: `3.5rem`,
          }}
          initial={{ opacity: 0, y: 20, scale: 0.8 }}
          animate={{ opacity: 1, y: 0, scale: 1 }}
          exit={{ opacity: 0, y: 20, scale: 0.8 }}
          transition={{ type: "spring", stiffness: 300, damping: 30 }}
        >
          {!ready &&
          (!duplicates ||
            duplicates.length <= 0 ||
            !duplicates.some(
              (group) =>
                group.map((item) => mediaByName.get(item)).filter(Boolean)
                  .length > 1,
            )) ? (
            <div className="flex h-full flex-col gap-4 p-4">
              {Array.from({ length: 6 }).map((_, index) => (
                <div key={index} className="flex w-full gap-4">
                  {Array.from({
                    length: Math.floor(Math.random() * 5) + 1,
                  }).map((_, i) => (
                    <div
                      key={i}
                      className="bg-background/60 border-border/50 aspect-square w-31 animate-pulse rounded-lg border"
                      style={{
                        animationDelay: `${(i / (5 * 5 - 1)) * 1.5}s`,
                      }}
                    />
                  ))}
                </div>
              ))}
            </div>
          ) : duplicates &&
            duplicates.length > 0 &&
            duplicates.some(
              (group) =>
                group.map((item) => mediaByName.get(item)).filter(Boolean)
                  .length > 1,
            ) ? (
            <div className="flex flex-col gap-2 p-4">
              {duplicates.map(
                (group, index) =>
                  group.length > 1 && (
                    <div
                      key={index}
                      className="border-border/60 bg-background/60 relative overflow-hidden rounded-2xl border p-3 shadow-sm"
                    >
                      <div className="mb-2">
                        <Button
                          variant="outline"
                          size="sm"
                          onClick={async () => {
                            if (!albumSlice?.path) return;
                            await markNonDuplicates(albumSlice.path, group);
                            setDuplicates((prev) =>
                              prev.filter((_, i) => i !== index),
                            );
                          }}
                        >
                          {t("duplicates.markNonDuplicates")}
                        </Button>
                      </div>
                      <div className="relative">
                        {group.length > 5 ? (
                          <ScrollRightFade className="flex gap-3 overflow-x-auto pb-2">
                            {group.map((item) => {
                              const image = mediaByName.get(item);
                              if (!image) return null;
                              return (
                                <div key={image.name} className="w-31 shrink-0">
                                  <Duplicate
                                    image={image}
                                    onDelete={() => {
                                      void (async () => {
                                        await deleteMedia(image);
                                      })();
                                    }}
                                  />
                                </div>
                              );
                            })}
                          </ScrollRightFade>
                        ) : (
                          <div className="flex flex-wrap gap-3">
                            {group.map((item) => {
                              const image = mediaByName.get(item);
                              if (!image) return null;
                              return (
                                <div key={image.name} className="w-31 shrink-0">
                                  <Duplicate
                                    image={image}
                                    onDelete={() => {
                                      void (async () => {
                                        await deleteMedia(image);
                                      })();
                                    }}
                                  />
                                </div>
                              );
                            })}
                          </div>
                        )}
                      </div>
                    </div>
                  ),
              )}
            </div>
          ) : (
            <div className="p-4 text-center">
              <span className="text-foreground/80">{t("duplicates.none")}</span>
            </div>
          )}
        </motion.div>
      )}
    </AnimatePresence>
  );
}
