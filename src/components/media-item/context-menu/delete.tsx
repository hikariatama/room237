"use client";

import { Button } from "@/components/ui/button";
import {
  ContextMenuItem,
  ContextMenuSub,
  ContextMenuSubContent,
  ContextMenuSubTrigger,
} from "@/components/ui/context-menu";
import { useUpload } from "@/lib/hooks/use-upload";
import { useI18n } from "@/lib/i18n";
import { useRoom237 } from "@/lib/stores";
import { extractItemFromState } from "@/lib/utils";
import { IconTrash } from "@tabler/icons-react";
import { useCallback, useEffect, useState } from "react";

export function DeleteContextMenuItem({ mediaPath }: { mediaPath: string }) {
  const { t } = useI18n();
  const { deleteMedias } = useUpload();

  const [open, setOpen] = useState(false);

  const selectionCount = useRoom237((state) => state.selection.length || 1);

  const handleDelete = useCallback(async () => {
    const state = useRoom237.getState();
    const selection = state.selection;
    const item = extractItemFromState({ state, path: mediaPath });
    const todo = selection.length > 0 ? selection : item ? [item] : [];
    if (todo.length === 0) return;
    await deleteMedias(todo);
  }, [deleteMedias, mediaPath]);

  useEffect(() => {
    console.log(open);
  }, [open]);

  return (
    <ContextMenuSub
      open={open}
      onOpenChange={(next) => !next && setOpen(false)}
    >
      <ContextMenuSubTrigger
        className="gap-2 text-red-500"
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
          <IconTrash className="size-4" />
          {selectionCount > 1
            ? t("contextMenu.deleteItems", { count: selectionCount })
            : t("contextMenu.delete")}
        </Button>
      </ContextMenuSubTrigger>
      <ContextMenuSubContent>
        <div key="delete-menu" className="space-y-1">
          <p className="pt-2 text-center text-sm font-semibold text-red-500">
            {t("contextMenu.deleteItems", { count: selectionCount })}
          </p>
          <div className="flex justify-end gap-2 px-1 pb-1">
            <ContextMenuItem className="font-semibold">
              {t("contextMenu.cancel")}
            </ContextMenuItem>
            <ContextMenuItem
              className="font-semibold text-red-500"
              onClick={() => handleDelete()}
            >
              {t("contextMenu.delete")}
            </ContextMenuItem>
          </div>
        </div>
      </ContextMenuSubContent>
    </ContextMenuSub>
  );
}
