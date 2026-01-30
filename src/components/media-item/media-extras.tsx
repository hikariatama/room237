"use client";

import { Button } from "@/components/ui/button";
import { useUpload } from "@/lib/hooks/use-upload";
import { useViewer } from "@/lib/hooks/use-viewer";
import { useRoom237 } from "@/lib/stores";
import { cn, copyFiles, extractItemFromState } from "@/lib/utils";
import { AnimatePresence, motion } from "framer-motion";
import {
  IconClipboard,
  IconHeart,
  IconLoader2,
  IconTrash,
  IconX,
} from "@tabler/icons-react";
import {
  memo,
  useCallback,
  useLayoutEffect,
  useRef,
  useState,
  type MouseEvent as ReactMouseEvent,
} from "react";
import { useI18n } from "@/lib/i18n";
import { toast } from "../toaster";
import { useStoreWithEqualityFn } from "zustand/traditional";
import { isEqual } from "lodash";

export const MediaExtras = memo(function MediaExtras({
  mediaPath,
}: {
  mediaPath: string;
}) {
  const itemData = useStoreWithEqualityFn(
    useRoom237,
    (state) => {
      const item = extractItemFromState({ state, path: mediaPath });
      return {
        name: item?.name,
        path: item?.path,
        meta: item?.meta,
        favorite: item?.favorite,
        index: item?.index,
      };
    },
    isEqual,
  );
  const [dateScale, setDateScale] = useState(1);
  const [confirm, setConfirm] = useState(false);
  const [copying, setCopying] = useState(false);
  const dateRef = useRef<HTMLDivElement>(null);
  const favoriteRef = useRef<HTMLButtonElement>(null);

  const onSelectToggle = useRoom237((state) => state.toggleSelection);
  const onSelectRange = useRoom237((state) => state.selectRange);
  const showExtras = useRoom237((state) => state.columns <= 10);
  const language = useRoom237((state) => state.language);
  const { t } = useI18n();

  const viewer = useViewer();
  const { deleteMedia: onRequestDelete, toggleFavorite: onToggleFavorite } =
    useUpload();

  const onView = useCallback(() => {
    if (itemData.index !== undefined) viewer.open(itemData.index);
  }, [viewer, itemData.index]);

  const click = useCallback(
    (e: ReactMouseEvent<HTMLDivElement>) => {
      const add = e.metaKey || e.ctrlKey;
      const shift = e.shiftKey;

      if (shift) {
        e.preventDefault();
        const state = useRoom237.getState();
        const item = extractItemFromState({ state, path: mediaPath });
        if (item) onSelectRange(item);
        return;
      }

      if (add) {
        e.preventDefault();
        const state = useRoom237.getState();
        const item = extractItemFromState({ state, path: mediaPath });
        if (item) onSelectToggle(item, true);
        return;
      }
      onView();
    },
    [mediaPath, onView, onSelectToggle, onSelectRange],
  );

  let dateTimestamp: number | undefined;
  if (itemData.meta?.shoot) dateTimestamp = itemData.meta.shoot * 1000;
  else if (itemData.meta?.added) dateTimestamp = itemData.meta.added * 1000;

  const dateText = useRoom237((state) =>
    dateTimestamp
      ? new Date(dateTimestamp).toLocaleDateString(
          language === "ru" ? "ru-RU" : "en-US",
          state.columns >= 9
            ? {
                month: "2-digit",
                day: "2-digit",
                year: "2-digit",
              }
            : {
                year: "numeric",
                month: "short",
                day: "2-digit",
              },
        )
      : t("media.unknownDate"),
  );

  useLayoutEffect(() => {
    const measure = () => {
      const badge = dateRef.current;
      if (!badge) return;
      const parent = badge.offsetParent?.parentNode;
      if (!(parent instanceof HTMLElement)) return;
      const favWidth = favoriteRef.current?.getBoundingClientRect().width ?? 0;
      const margin = 18;
      const maxWidth = parent.clientWidth - favWidth - margin;
      if (maxWidth <= 0) {
        setDateScale(0);
        return;
      }
      const badgeWidth =
        badge.scrollWidth ?? badge.getBoundingClientRect().width;
      if (!badgeWidth) return;
      const nextScale = Math.min(1, maxWidth / badgeWidth);
      setDateScale(nextScale);
    };

    measure();
    const parent = dateRef.current?.offsetParent?.parentNode;
    if (!(parent instanceof HTMLElement)) return;
    if (typeof ResizeObserver === "undefined") return;
    const ro = new ResizeObserver(measure);
    ro.observe(parent);
    if (dateRef.current) ro.observe(dateRef.current);
    return () => ro.disconnect();
  }, [dateText]);

  return (
    <>
      <div
        className="absolute top-0 right-0 bottom-0 left-0 z-10 m-auto h-full w-full cursor-pointer"
        onClick={click}
      />
      {showExtras && (
        <div className="pointer-events-none absolute top-2 left-2 flex flex-col gap-1">
          <div
            ref={dateRef}
            className="text-foreground rounded-md bg-black/70 px-2 py-0.5 opacity-0 backdrop-blur-lg transition-all duration-150 group-hover:opacity-100"
            style={{
              transform: `scale(${dateScale})`,
              transformOrigin: "left top",
              maxWidth: "100%",
            }}
          >
            <span className="block truncate text-xs leading-tight">
              {dateText}
            </span>
          </div>
        </div>
      )}

      {showExtras ? (
        <motion.button
          ref={favoriteRef}
          whileHover={{ scale: 1.1 }}
          whileTap={{ scale: 0.9 }}
          transition={{ type: "spring", stiffness: 500, damping: 25 }}
          className={cn(
            "group-hover:bg-background/70 absolute top-1 right-1 z-30 flex h-6 w-6 cursor-pointer items-center justify-center rounded-md transition-[background-color,opacity,backdrop-filter] duration-150 group-hover:backdrop-blur-sm",
            itemData.favorite && "text-red-500",
            !itemData.favorite && "opacity-0 group-hover:opacity-100",
          )}
          onClick={(e) => {
            e.stopPropagation();
            const state = useRoom237.getState();
            const item = extractItemFromState({ state, path: mediaPath });
            if (item) void onToggleFavorite(item);
          }}
          onPointerDownCapture={(e) => e.stopPropagation()}
        >
          <IconHeart
            className="h-3.5 w-3.5 transition-colors duration-150"
            fill={itemData.favorite ? "currentColor" : "none"}
          />
        </motion.button>
      ) : itemData.favorite ? (
        <div className="absolute top-1 right-1 z-30 flex h-4 w-4 items-center justify-center rounded-md text-red-500">
          <IconHeart
            className="h-3 w-3 transition-colors duration-150"
            fill="currentColor"
          />
        </div>
      ) : null}

      {showExtras && (
        <div className="pointer-events-none absolute bottom-0 flex w-full items-end justify-end p-2 opacity-0 transition-opacity duration-150 group-hover:opacity-100">
          <div className="z-30 flex gap-1">
            <motion.button
              whileHover={{ scale: 1.1 }}
              whileTap={{ scale: 0.9 }}
              transition={{ type: "spring", stiffness: 500, damping: 25 }}
              className="bg-background/70 pointer-events-auto flex h-7 w-7 cursor-pointer items-center justify-center rounded-md backdrop-blur-sm hover:text-red-500"
              onClick={(e) => {
                e.stopPropagation();
                setConfirm(true);
              }}
              onPointerDownCapture={(e) => e.stopPropagation()}
            >
              <IconTrash className="h-4 w-4" />
            </motion.button>
            <motion.button
              whileHover={copying ? {} : { scale: 1.1 }}
              whileTap={copying ? {} : { scale: 0.9 }}
              transition={{ type: "spring", stiffness: 500, damping: 25 }}
              className="bg-background/70 pointer-events-auto flex h-7 w-7 cursor-pointer items-center justify-center rounded-md backdrop-blur-sm"
              onClick={async (e) => {
                e.stopPropagation();
                setCopying(true);
                try {
                  const state = useRoom237.getState();
                  const item = extractItemFromState({
                    state,
                    path: mediaPath,
                  });
                  if (!item) return;
                  await copyFiles([item]);
                } catch (err) {
                  console.error(err);
                  toast.error(t("media.copyFailed"));
                } finally {
                  setCopying(false);
                }
              }}
              onPointerDownCapture={(e) => e.stopPropagation()}
              disabled={copying}
            >
              {copying ? (
                <IconLoader2 className="text-muted-foreground size-4 animate-spin" />
              ) : (
                <IconClipboard className="size-4" />
              )}
            </motion.button>
          </div>
        </div>
      )}

      <AnimatePresence>
        {confirm && (
          <motion.div
            initial={{ opacity: 0 }}
            animate={{ opacity: 1 }}
            exit={{ opacity: 0 }}
            className="bg-background/70 absolute inset-0 z-50 flex flex-col items-center justify-center gap-2 backdrop-blur-sm"
          >
            <div className="flex gap-1">
              <Button
                size="sm"
                onClick={() => {
                  const state = useRoom237.getState();
                  const item = extractItemFromState({ state, path: mediaPath });
                  if (item) void onRequestDelete(item);
                }}
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
    </>
  );
});
