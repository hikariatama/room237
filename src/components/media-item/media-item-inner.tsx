/* eslint-disable @next/next/no-img-element */
"use client";

import { MAX_COLS } from "@/lib/consts";
import { useDragDrop } from "@/lib/hooks/use-drag-drop";
import { useRoom237 } from "@/lib/stores";
import {
  cancelIdle,
  cn,
  isVideo,
  requestIdle,
  extractItemFromState,
} from "@/lib/utils";
import { motion } from "framer-motion";
import { IconPlayerPlay } from "@tabler/icons-react";
import { memo, startTransition, useEffect, useMemo, useState } from "react";
import { MediaExtras } from "./media-extras";
import { useStoreWithEqualityFn } from "zustand/traditional";
import { isEqual } from "lodash";

export const MediaItemInner = memo(function MediaItemInner({
  mediaPath,
  className,
  imgClassName,
}: {
  mediaPath: string;
  className?: string;
  imgClassName?: string;
}) {
  const itemData = useStoreWithEqualityFn(
    useRoom237,
    (state) => {
      const item = extractItemFromState({ state, path: mediaPath });
      return {
        name: item?.name,
        thumb: item?.thumb,
        favorite: item?.favorite,
      };
    },
    isEqual,
  );

  const selected = useRoom237((state) =>
    state.selection.some((i) => i.path === mediaPath),
  );

  const { onDragStart, clear } = useDragDrop();
  const columns = useRoom237((state) => state.columns);

  const itemStyle = useMemo(
    () => ({
      borderRadius: `${(1 - columns / MAX_COLS) * 0.75 + 0.15}rem`,
      fontSize: `${(1 - columns / MAX_COLS) * 4 + 8}px`,
    }),
    [columns],
  );

  const [deferredExtrasMounted, setDeferredExtrasMounted] = useState(false);

  useEffect(() => {
    const id = requestIdle(() => {
      startTransition(() => setDeferredExtrasMounted(true));
    });
    return () => cancelIdle(id);
  }, []);

  const showExtras = useMemo(() => columns <= 10, [columns]);

  if (!itemData.name) return null;

  return (
    <motion.div
      data-img-url={itemData.name}
      exit={{ opacity: 0, y: -300, transition: { duration: 0.15 } }}
      whileHover={{ scale: 1.027 }}
      whileTap={{ scale: 0.98 }}
      transition={{ type: "spring", stiffness: 600, damping: 25 }}
      className={cn(
        "group border-border/50 bg-background/40 relative mb-2 break-inside-avoid overflow-hidden rounded-md border text-xs shadow-sm transition-shadow duration-200 select-none",
        className,
        selected && "shadow-md ring-2 shadow-blue-600/50 ring-blue-500",
      )}
      draggable
      onDragStart={(e) => {
        const state = useRoom237.getState();
        const item = extractItemFromState({ state, path: mediaPath });
        if (item) onDragStart(e, item);
      }}
      onDragEnd={clear}
      style={itemStyle}
    >
      {deferredExtrasMounted && showExtras && (
        <MediaExtras mediaPath={mediaPath} />
      )}

      {isVideo(itemData.name) ? (
        <>
          <img
            src={itemData.thumb}
            alt={itemData.name}
            className={cn(
              "block w-full cursor-pointer select-none",
              imgClassName,
            )}
          />
          <IconPlayerPlay className="pointer-events-none absolute top-1/2 left-1/2 h-8 w-8 -translate-x-1/2 -translate-y-1/2 text-white/80" />
        </>
      ) : (
        <img
          src={itemData.thumb}
          alt={itemData.name}
          className={cn(
            "block w-full cursor-pointer select-none",
            imgClassName,
          )}
          onError={(e) => {
            const target = e.target as HTMLImageElement;
            const retryCount = parseInt(target.dataset.retryCount ?? "0");

            if (retryCount < 15) {
              setTimeout(() => {
                target.dataset.retryCount = (retryCount + 1).toString();
                const url = new URL(target.src, location.href);
                url.searchParams.set("_retry", Date.now().toString());
                target.src = url.toString();
              }, 1000);
            }
          }}
        />
      )}
      {itemData.favorite && (
        <div className="absolute bottom-1 left-1 flex size-5 items-center justify-center rounded-full bg-black/50 backdrop-blur-sm">
          <svg
            xmlns="http://www.w3.org/2000/svg"
            width="12"
            height="12"
            viewBox="0 0 24 24"
            fill="currentColor"
            className="text-red-500"
          >
            <path d="M12 21.35l-1.45-1.32C5.4 15.36 2 12.28 2 8.5 2 5.42 4.42 3 7.5 3c1.74 0 3.41.81 4.5 2.09C13.09 3.81 14.76 3 16.5 3 19.58 3 22 5.42 22 8.5c0 3.78-3.4 6.86-8.55 11.54L12 21.35z" />
          </svg>
        </div>
      )}
    </motion.div>
  );
});
