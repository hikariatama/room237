"use client";

import { Button } from "@/components/ui/button";
import {
  Popover,
  PopoverContent,
  PopoverTrigger,
} from "@/components/ui/popover";
import { FAVORITES_ALBUM_ID } from "@/lib/consts";
import { useI18n } from "@/lib/i18n";
import { useRoom237 } from "@/lib/stores";
import { cn } from "@/lib/utils";
import { IconStack2, IconTrash } from "@tabler/icons-react";
import { AnimatePresence, motion } from "framer-motion";
import { useState } from "react";
import { useStoreWithEqualityFn } from "zustand/traditional";

export function AlbumActions() {
  const duplicatesAvailable = useRoom237((state) => state.duplicatesAvailable);
  const duplicatesLoading = useRoom237((state) => state.duplicatesLoading);
  const showDuplicates = useRoom237((state) => state.showDuplicates);
  const setShowDuplicates = useRoom237((state) => state.setShowDuplicates);
  const deleteAlbum = useRoom237((state) => state.deleteAlbum);
  const [deleteOpen, setDeleteOpen] = useState(false);
  const { t } = useI18n();

  const activeAlbum = useStoreWithEqualityFn(
    useRoom237,
    (state) => {
      if (!state.activeAlbumId) return null;

      if (
        state.activeAlbumId === "Favorites" ||
        state.activeAlbumId === FAVORITES_ALBUM_ID
      ) {
        return state.favoritesAlbum
          ? {
              id: state.favoritesAlbum.albumId,
              name: state.favoritesAlbum.name,
              favorite: true,
            }
          : null;
      }

      return state.albumsById[state.activeAlbumId]
        ? {
            id: state.albumsById[state.activeAlbumId]!.albumId,
            name: state.albumsById[state.activeAlbumId]!.name,
            favorite: false,
          }
        : null;
    },
    (a, b) =>
      a?.id === b?.id && a?.name === b?.name && a?.favorite === b?.favorite,
  );

  if (!activeAlbum || activeAlbum.favorite) {
    return null;
  }

  return (
    <div className="flex items-stretch gap-2">
      <div className="relative flex w-px flex-col justify-center">
        <AnimatePresence>
          {duplicatesLoading && (
            <motion.div
              key="duplicates-loading"
              initial={{ opacity: 0, scale: 1, y: -16 }}
              animate={{ opacity: 1, scale: 1, y: 0 }}
              exit={{ opacity: 0, scale: 0.7, y: -16 }}
              transition={{
                duration: 0.1,
                scale: { type: "spring", stiffness: 300, damping: 20 },
              }}
              className="text-muted-foreground absolute right-0 flex h-fit items-center gap-1.5 rounded-full border py-1 pr-3 pl-2 text-xs whitespace-pre"
            >
              <svg
                width="24"
                height="24"
                viewBox="0 0 24 24"
                fill="none"
                stroke="currentColor"
                strokeWidth="2"
                strokeLinecap="round"
                strokeLinejoin="round"
                className="size-3"
              >
                <path d="M10.7 20H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h3.9a2 2 0 0 1 1.69.9l.81 1.2a2 2 0 0 0 1.67.9H20a2 2 0 0 1 2 2v4.1" />
                <motion.g
                  animate={{
                    x: [2, 0, -2, 0, 2],
                    y: [0, 2, 0, -2, 0],
                  }}
                  transition={{
                    duration: 1,
                    ease: "linear",
                    repeat: Infinity,
                  }}
                >
                  <path d="m21 21-1.9-1.9" />
                  <circle cx="17" cy="17" r="3" />
                </motion.g>
              </svg>
              {t("duplicates.loading")}
            </motion.div>
          )}
          {!duplicatesLoading && duplicatesAvailable && (
            <motion.div
              key="duplicates-button"
              initial={{ opacity: 0, scale: 0.7, y: 16 }}
              animate={{ opacity: 1, scale: 1, y: 0 }}
              exit={{ opacity: 0, scale: 0.7, y: 16 }}
              transition={{
                duration: 0.1,
                scale: { type: "spring", stiffness: 300, damping: 20 },
              }}
              className="absolute right-0"
            >
              <Button
                variant="outline"
                onClick={() => setShowDuplicates(!showDuplicates)}
                className={cn(
                  "transition-shadow duration-200",
                  showDuplicates && "shadow-[0_0_5px_0_var(--color-green-400)]",
                )}
              >
                <IconStack2 />
                <span>
                  {showDuplicates ? t("duplicates.hide") : t("duplicates.show")}
                </span>
              </Button>
            </motion.div>
          )}
        </AnimatePresence>
      </div>
      <motion.div
        key="delete-album-button"
        initial={{ opacity: 0 }}
        animate={{ opacity: 1 }}
        exit={{ opacity: 0 }}
        transition={{ duration: 0.1 }}
      >
        <Popover open={deleteOpen} onOpenChange={setDeleteOpen}>
          <PopoverTrigger asChild>
            <Button variant="destructive" onClick={() => setDeleteOpen(true)}>
              <IconTrash className="text-red-500" />
              {t("album.deleteTitle")}
            </Button>
          </PopoverTrigger>
          <PopoverContent>
            <p className="text-secondary-foreground mb-4 text-sm">
              <span
                dangerouslySetInnerHTML={{
                  __html: t("album.deleteConfirm", {
                    values: { album: activeAlbum?.name ?? "" },
                  }),
                }}
              />
            </p>
            <p className="mb-4 text-sm font-bold text-red-500">
              {t("album.deleteWarning")}
            </p>
            <div className="flex gap-2">
              <Button
                variant="destructive"
                onClick={() => {
                  if (!activeAlbum) return;
                  void deleteAlbum(activeAlbum.id);
                }}
                className="flex-auto"
              >
                <IconTrash className="text-red-500" />
                {t("common.delete")}
              </Button>
              <Button variant="secondary" onClick={() => setDeleteOpen(false)}>
                {t("common.cancel")}
              </Button>
            </div>
          </PopoverContent>
        </Popover>
      </motion.div>
    </div>
  );
}
