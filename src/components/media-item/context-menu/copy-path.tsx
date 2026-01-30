"use client";

import { toast } from "@/components/toaster";
import { Button } from "@/components/ui/button";
import { ContextMenuItem } from "@/components/ui/context-menu";
import { useI18n } from "@/lib/i18n";
import { IconLink } from "@tabler/icons-react";
import { useCallback } from "react";

export function CopyPathContextMenuItem({ mediaPath }: { mediaPath: string }) {
  const { t } = useI18n();

  const handleCopyPath = useCallback(async () => {
    try {
      await navigator.clipboard.writeText(mediaPath);
      toast.success("Path copied");
    } catch (error) {
      console.error(error);
      toast.error("Failed to copy path");
    }
  }, [mediaPath]);

  return (
    <ContextMenuItem
      className="gap-2"
      onPointerMove={(e) => e.preventDefault()}
      onPointerLeave={(e) => e.preventDefault()}
      onClick={() => handleCopyPath()}
      asChild
    >
      <Button variant="ghost" className="w-full justify-start">
        <IconLink className="size-4" />
        {t("contextMenu.copyPath")}
      </Button>
    </ContextMenuItem>
  );
}
