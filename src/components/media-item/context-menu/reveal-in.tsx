"use client";

import { toast } from "@/components/toaster";
import { Button } from "@/components/ui/button";
import { ContextMenuItem } from "@/components/ui/context-menu";
import { useFileManagerName } from "@/lib/hooks/use-file-manager-name";
import { useI18n } from "@/lib/i18n";
import { getFileManagerIcon } from "@/lib/utils";
import { revealItemInDir } from "@tauri-apps/plugin-opener";
import { useCallback, useMemo } from "react";

export function RevealInContextMenuItem({ mediaPath }: { mediaPath: string }) {
  const { t } = useI18n();

  const fileManagerName = useFileManagerName() ?? "file manager";
  const fileManagerIcon = useMemo(
    () => getFileManagerIcon(fileManagerName),
    [fileManagerName],
  );

  const handleReveal = useCallback(async () => {
    try {
      await revealItemInDir(mediaPath);
    } catch (error) {
      console.error(error);
      toast.error("Failed to reveal in file manager");
    }
  }, [mediaPath]);

  return (
    <ContextMenuItem
      className="gap-2"
      onPointerMove={(e) => e.preventDefault()}
      onPointerLeave={(e) => e.preventDefault()}
      onClick={() => handleReveal()}
      asChild
    >
      <Button variant="ghost" className="w-full justify-start">
        {fileManagerIcon}
        {t("contextMenu.revealIn", {
          values: { fileManager: fileManagerName },
        })}
      </Button>
    </ContextMenuItem>
  );
}
