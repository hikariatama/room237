"use client";

import { Button } from "@/components/ui/button";
import { useSortedMedia } from "@/lib/hooks/use-sorted-media";
import { useRoom237 } from "@/lib/stores";
import { cn } from "@/lib/utils";
import { AnimatePresence, motion } from "framer-motion";
import { useMemo } from "react";
import { IconHeart } from "@tabler/icons-react";
import { useI18n } from "@/lib/i18n";
import { useStoreWithEqualityFn } from "zustand/traditional";

export function FavoritesFilter() {
  const favoritesOnly = useRoom237((state) => state.favoritesOnly);
  const toggleFavoritesOnly = useRoom237((state) => state.toggleFavoritesOnly);
  const activeAlbum = useStoreWithEqualityFn(
    useRoom237,
    (state) => {
      if (!state.activeAlbumId) return null;
      const album = state.albumsById[state.activeAlbumId];
      if (!album) return null;
      return {
        id: album.albumId,
        path: album.path,
      };
    },
    (a, b) => a?.id === b?.id && a?.path === b?.path,
  );
  const { mediaArray } = useSortedMedia();
  const { t } = useI18n();

  const activeAlbumHasFavorites = useMemo(() => {
    if (!activeAlbum) return false;
    return mediaArray.some((media) => media.favorite);
  }, [activeAlbum, mediaArray]);

  if (!activeAlbum || !activeAlbumHasFavorites) {
    return null;
  }

  return (
    <AnimatePresence>
      <motion.div
        initial={{ width: 0, opacity: 0 }}
        animate={{ width: "auto", opacity: 1 }}
        exit={{ width: 0, opacity: 0 }}
        transition={{ duration: 0.2 }}
      >
        <Button
          variant="outline"
          className={cn(favoritesOnly && "border-red-500 text-red-500")}
          onClick={() => activeAlbum && toggleFavoritesOnly(activeAlbum.id)}
          title={favoritesOnly ? t("favorites.showAll") : t("favorites.show")}
        >
          <IconHeart fill={favoritesOnly ? "currentColor" : "none"} />
        </Button>
      </motion.div>
    </AnimatePresence>
  );
}
