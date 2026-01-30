"use client";

import {
  ContextMenu,
  ContextMenuContent,
  ContextMenuTrigger,
} from "@/components/ui/context-menu";
import { useRoom237 } from "@/lib/stores";
import { cancelIdle, extractItemFromState, requestIdle } from "@/lib/utils";
import { AnimatePresence } from "framer-motion";
import { memo, startTransition, useEffect, useState } from "react";
import { MediaItemInner } from "./media-item-inner";
import { MediaItemContextMenuContents } from "./context-menu";

export const MediaItem = memo(function MediaItem({
  mediaPath,
  className,
  imgClassName,
}: {
  mediaPath: string;
  className?: string;
  imgClassName?: string;
}) {
  const [contextMenuOpen, setContextMenuOpen] = useState(false);
  const itemExists = useRoom237((state) => {
    const item = extractItemFromState({ state, path: mediaPath });
    return item !== undefined;
  });

  const [deferredContextMenuMounted, setDeferredContextMenuMounted] =
    useState(false);
  useEffect(() => {
    const id = requestIdle(() => {
      startTransition(() => setDeferredContextMenuMounted(true));
    });
    return () => cancelIdle(id);
  }, []);

  if (!itemExists) return null;
  if (!deferredContextMenuMounted) {
    return (
      <MediaItemInner
        mediaPath={mediaPath}
        className={className}
        imgClassName={imgClassName}
      />
    );
  }

  return (
    <ContextMenu open={contextMenuOpen} onOpenChange={setContextMenuOpen}>
      <ContextMenuTrigger asChild>
        <div>
          <MediaItemInner
            mediaPath={mediaPath}
            className={className}
            imgClassName={imgClassName}
          />
        </div>
      </ContextMenuTrigger>
      <ContextMenuContent>
        <AnimatePresence mode="popLayout" initial={false}>
          {contextMenuOpen && (
            <MediaItemContextMenuContents mediaPath={mediaPath} />
          )}
        </AnimatePresence>
      </ContextMenuContent>
    </ContextMenu>
  );
});
