"use client";

import { toast } from "@/components/toaster";
import { Button } from "@/components/ui/button";
import { ContextMenuItem } from "@/components/ui/context-menu";
import { useI18n } from "@/lib/i18n";
import { useRoom237 } from "@/lib/stores";
import { copyFiles, extractItemFromState } from "@/lib/utils";
import { IconCopy } from "@tabler/icons-react";
import { useCallback } from "react";

export function CopyImageContextMenuItem({ mediaPath }: { mediaPath: string }) {
  const { t } = useI18n();
  const selection = useRoom237((state) => state.selection);

  const handleCopyImage = useCallback(async () => {
    try {
      const state = useRoom237.getState();

      if (selection.length > 1) {
        await copyFiles(selection);
      } else {
        const item = extractItemFromState({ state, path: mediaPath });
        if (!item) throw new Error("Media item not found");
        await copyFiles([item]);
      }
    } catch (error) {
      console.error(error);
      toast.error("Failed to copy");
    }
  }, [mediaPath, selection]);

  const label =
    selection.length > 1
      ? t("contextMenu.copyFiles", { count: selection.length })
      : t("contextMenu.copyFile");

  return (
    <ContextMenuItem
      className="gap-2"
      onPointerMove={(e) => e.preventDefault()}
      onPointerLeave={(e) => e.preventDefault()}
      onClick={() => handleCopyImage()}
      asChild
    >
      <Button variant="ghost" className="w-full justify-start">
        <IconCopy className="size-4" />
        {label}
      </Button>
    </ContextMenuItem>
  );
}
