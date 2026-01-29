"use client";

import { ContextMenuSeparator } from "@/components/ui/context-menu";
import { useRoom237 } from "@/lib/stores";
import { ChangeDateContextMenuItem } from "./change-date";
import { CopyImageContextMenuItem } from "./copy-image";
import { CopyPathContextMenuItem } from "./copy-path";
import { DeleteContextMenuItem } from "./delete";
import { MoveToAlbumMenuItem } from "./move-to-album";
import { RevealInContextMenuItem } from "./reveal-in";

export function MediaItemContextMenuContents({
  mediaPath,
}: {
  mediaPath: string;
}) {
  const selectionCount = useRoom237((state) => state.selection.length);

  if (selectionCount > 1) {
    return (
      <>
        <CopyImageContextMenuItem mediaPath={mediaPath} />
        <ContextMenuSeparator />
        <ChangeDateContextMenuItem mediaPath={mediaPath} />
        <MoveToAlbumMenuItem mediaPath={mediaPath} />
        <ContextMenuSeparator />
        <DeleteContextMenuItem mediaPath={mediaPath} />
      </>
    );
  }

  return (
    <>
      <CopyImageContextMenuItem mediaPath={mediaPath} />
      <RevealInContextMenuItem mediaPath={mediaPath} />
      <ChangeDateContextMenuItem mediaPath={mediaPath} />
      <CopyPathContextMenuItem mediaPath={mediaPath} />
      <MoveToAlbumMenuItem mediaPath={mediaPath} />
      <ContextMenuSeparator />
      <DeleteContextMenuItem mediaPath={mediaPath} />
    </>
  );
}
