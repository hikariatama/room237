import { useMemo, useState } from "react";
import { useRoom237 } from "@/lib/stores";
import { useI18n } from "@/lib/i18n";
import { HashingProgress, useHashingStatus } from "./hashing-progress";
import { Kbd } from "../ui/kbd";
import { cn } from "@/lib/utils";
import { AnimatePresence, motion } from "framer-motion";
import { Button } from "../ui/button";

function HotkeyHints() {
  const { t } = useI18n();
  const hasSelection = useRoom237((state) => state.selection.length > 0);
  const albumSelected = useRoom237((state) => !!state.activeAlbumId);
  const [open, setOpen] = useState(false);

  const hints = useMemo(() => {
    return [
      ...(hasSelection
        ? [
            {
              keys: ["meta", "d"],
              label: t("hotkeys.clearSelection"),
              id: "clear-selection",
            },
            {
              keys: ["meta", "c"],
              label: t("hotkeys.copyToClipboard"),
              id: "copy-to-clipboard",
            },
            {
              keys: ["delete"],
              label: t("hotkeys.deleteSelection"),
              id: "delete-selection",
            },
          ]
        : []),
      ...(albumSelected
        ? [
            {
              keys: ["meta", "click"],
              label: t("hotkeys.selectPhoto"),
              id: "select-photo",
            },
            {
              keys: ["meta", "a"],
              label: t("hotkeys.selectAll"),
              id: "select-all",
            },
            {
              keys: ["meta", "v"],
              label: t("hotkeys.pasteFromClipboard"),
              id: "paste-from-clipboard",
            },
          ]
        : []),
      {
        keys: ["rightclick"],
        label: t("hotkeys.moreActions"),
        id: "more-actions",
      },
    ];
  }, [hasSelection, t, albumSelected]);

  if (!open) {
    return (
      <Button variant="outline" size="sm" onClick={() => setOpen(true)}>
        {t("hotkeys.show")}
      </Button>
    );
  }

  return (
    <div className="z-30 flex flex-col gap-1.5 rounded-tr-2xl bg-[#151414] pt-2 pr-2">
      <AnimatePresence>
        {hints.map((hint, index) => (
          <motion.div
            key={hint.id}
            className="flex items-center gap-2"
            initial={{ opacity: 0, x: -10 }}
            animate={{ opacity: 1, x: 0 }}
            exit={{ opacity: 0, x: -10 }}
            transition={{ delay: index * 0.05 }}
          >
            <Kbd keys={hint.keys} variant="secondary" />
            <span className="text-muted-foreground text-xs">{hint.label}</span>
          </motion.div>
        ))}
      </AnimatePresence>
      <Button variant="outline" size="sm" onClick={() => setOpen(false)}>
        {t("hotkeys.hide")}
      </Button>
    </div>
  );
}

export function BottomLeftHelpers({ className }: { className?: string }) {
  const hashingState = useHashingStatus();

  return (
    <div className={cn("absolute bottom-4 left-4 z-10", className)}>
      {hashingState.active ? (
        <HashingProgress state={hashingState} />
      ) : (
        <HotkeyHints />
      )}
    </div>
  );
}
