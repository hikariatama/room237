"use client";

import { Button } from "@/components/ui/button";
import {
  ContextMenuItem,
  ContextMenuSeparator,
  ContextMenuSub,
  ContextMenuSubContent,
  ContextMenuSubTrigger,
} from "@/components/ui/context-menu";
import { FAVORITES_ALBUM_ID } from "@/lib/consts";
import { useUpload } from "@/lib/hooks/use-upload";
import { useI18n } from "@/lib/i18n";
import { useRoom237 } from "@/lib/stores";
import type { AlbumNode } from "@/lib/types/album";
import { extractItemFromState } from "@/lib/utils";
import { IconFolderOpen } from "@tabler/icons-react";
import { useCallback, useMemo, useState } from "react";

export function MoveToAlbumMenuItem({ mediaPath }: { mediaPath: string }) {
  const { t } = useI18n();
  const [open, setOpen] = useState(false);
  const { moveMediasToAlbum } = useUpload();
  const selectionCount = useRoom237((state) => state.selection.length || 1);

  const albumTree = useRoom237((state) => state.albumTree);
  const currentAlbumId = useRoom237((state) => {
    const item = extractItemFromState({ state, path: mediaPath });
    return item?.albumId;
  });

  const destinations = useMemo(() => {
    const list: { id: string; name: string; depth: number }[] = [];
    const walk = (nodes: AlbumNode[], depth: number) => {
      nodes.forEach((node) => {
        if (node.id === FAVORITES_ALBUM_ID) return;
        list.push({ id: String(node.id), name: node.name, depth });
        if (node.children.length) walk(node.children, depth + 1);
      });
    };
    walk(albumTree, 0);
    return list;
  }, [albumTree]);

  const handleMoveTo = useCallback(
    async (albumId: string) => {
      const state = useRoom237.getState();
      const { selection, albumsById } = state;
      const item = extractItemFromState({ state, path: mediaPath });
      const todo = selection.length > 0 ? selection : item ? [item] : [];
      if (todo.length === 0) return;
      const target = albumsById[albumId];
      if (!target || !item || target.albumId === item.albumId) return;
      await moveMediasToAlbum(target, todo);
    },
    [moveMediasToAlbum, mediaPath],
  );

  return (
    <ContextMenuSub
      open={open}
      onOpenChange={(next) => !next && setOpen(false)}
    >
      <ContextMenuSubTrigger
        className="gap-2"
        onPointerMove={(e) => e.preventDefault()}
        onPointerLeave={(e) => e.preventDefault()}
        onSelect={(e) => e.preventDefault()}
        onClick={(e) => {
          e.preventDefault();
          setOpen((prev) => !prev);
        }}
        asChild
      >
        <Button variant="ghost" className="w-full justify-start">
          <IconFolderOpen className="size-4" />
          {selectionCount > 1
            ? t("contextMenu.moveItems", { count: selectionCount })
            : t("contextMenu.moveToAlbum")}
        </Button>
      </ContextMenuSubTrigger>
      <ContextMenuSubContent>
        <div key="move-menu" className="space-y-2 p-1">
          <div className="max-h-72 overflow-y-auto">
            {destinations.length ? (
              destinations.map((dest) => (
                <ContextMenuItem
                  key={dest.id}
                  inset
                  disabled={dest.id === currentAlbumId}
                  style={{ paddingLeft: dest.depth * 12 + 12 }}
                  onSelect={(e) => {
                    e.preventDefault();
                    void handleMoveTo(dest.id);
                  }}
                >
                  <IconFolderOpen className="mr-2 size-4" />
                  {dest.name}
                </ContextMenuItem>
              ))
            ) : (
              <ContextMenuItem disabled>
                {t("contextMenu.noOtherAlbums")}
              </ContextMenuItem>
            )}
          </div>
          <ContextMenuSeparator />
          <ContextMenuItem>{t("contextMenu.cancel")}</ContextMenuItem>
        </div>
      </ContextMenuSubContent>
    </ContextMenuSub>
  );
}
