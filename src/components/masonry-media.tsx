/* eslint-disable @next/next/no-img-element */
"use client";

import type { MediaEntry } from "@/lib/types";
import { cn } from "@/lib/utils";
import { AnimatePresence, motion } from "framer-motion";
import { Trash2, ClipboardCopy, Play, Loader2 } from "lucide-react";
import {
  useState,
  type DragEvent as ReactDragEvent,
  type MouseEvent as ReactMouseEvent,
} from "react";
import { Button } from "./ui/button";
import { toast } from "sonner";

interface Props {
  item: MediaEntry;
  selected: boolean;
  onSelectToggle: (i: MediaEntry, add: boolean) => void;
  onDragStart: (
    e: MouseEvent | TouchEvent | PointerEvent | ReactDragEvent,
    i: MediaEntry,
  ) => void;
  onView: () => void;
  onRequestDelete: (i: MediaEntry) => void;
}

export const MasonryMedia: React.FC<Props> = ({
  item,
  selected,
  onSelectToggle,
  onDragStart,
  onView,
  onRequestDelete,
}) => {
  const [confirm, setConfirm] = useState(false);
  const [copying, setCopying] = useState(false);

  const click = (
    e: ReactMouseEvent<HTMLImageElement> | ReactMouseEvent<HTMLVideoElement>,
  ) => {
    const add = e.metaKey || e.ctrlKey;
    if (add) {
      e.preventDefault();
      onSelectToggle(item, true);
      return;
    }
    onView();
  };

  const copyFile = async () => {
    if (item.file.type === "image/png") {
      await navigator.clipboard.write([
        new ClipboardItem({ [item.file.type]: item.file }),
      ]);
      toast.success("Image copied to clipboard!");
      return;
    }
    if (!item.file.type.startsWith("image/")) return;
    const canvas = document.createElement("canvas");
    const ctx = canvas.getContext("2d");
    if (!ctx) return;
    const img = new Image();
    img.src = item.url();
    await new Promise((resolve) => {
      img.onload = resolve;
    });
    canvas.width = img.width;
    canvas.height = img.height;
    ctx.drawImage(img, 0, 0);
    const blob = await new Promise<Blob | null>((resolve) =>
      canvas.toBlob(resolve, "image/png"),
    );
    item.unload();
    if (!blob) return;
    await navigator.clipboard.write([new ClipboardItem({ "image/png": blob })]);
    toast.success("Image copied to clipboard!");
  };

  return (
    <motion.div
      data-img-url={item.thumb}
      initial={{ opacity: 0 }}
      animate={{ opacity: 1 }}
      exit={{ opacity: 0, y: -300, transition: { duration: 0.15 } }}
      className="group relative mb-4 break-inside-avoid select-none"
      draggable
      onDragStart={(e) => onDragStart(e, item)}
    >
      <div
        className={cn(
          "relative overflow-hidden rounded-md",
          selected && "ring-4 ring-blue-500",
        )}
      >
        {item.file.type.startsWith("video") ? (
          <>
            <img
              src={item.thumb}
              alt={item.file.name}
              className="block w-full cursor-pointer select-none"
              onClick={click}
            />
            <Play className="absolute top-1/2 left-1/2 h-8 w-8 -translate-x-1/2 -translate-y-1/2 text-white/80" />
          </>
        ) : (
          <img
            src={item.thumb}
            alt={item.file.name}
            className="block w-full cursor-pointer select-none"
            onClick={click}
          />
        )}

        <div className="absolute bottom-0 flex w-full items-end justify-between p-2 opacity-0 transition-opacity duration-150 group-hover:opacity-100">
          <div className="text-foreground rounded-md bg-black/70 px-2 py-0.5 text-xs backdrop-blur-lg">
            {item.meta.shoot || item.meta.added
              ? new Date(
                  item.meta.shoot ?? item.meta.added ?? 0,
                ).toLocaleDateString("en-US", {
                  year: "numeric",
                  month: "short",
                  day: "2-digit",
                })
              : "Unknown Date"}
          </div>
          <div className="flex gap-1">
            <motion.button
              whileHover={{ scale: 1.1 }}
              whileTap={{ scale: 0.9 }}
              transition={{ type: "spring", stiffness: 500, damping: 25 }}
              className="bg-background/70 flex h-7 w-7 items-center justify-center rounded-md backdrop-blur-sm hover:text-red-500"
              onClick={(e) => {
                e.stopPropagation();
                setConfirm(true);
              }}
            >
              <Trash2 className="h-4 w-4" />
            </motion.button>
            <motion.button
              whileHover={copying ? {} : { scale: 1.1 }}
              whileTap={copying ? {} : { scale: 0.9 }}
              transition={{ type: "spring", stiffness: 500, damping: 25 }}
              className="bg-background/70 flex h-7 w-7 items-center justify-center rounded-md backdrop-blur-sm"
              onClick={async (e) => {
                e.stopPropagation();
                setCopying(true);
                try {
                  await copyFile();
                } finally {
                  setCopying(false);
                }
              }}
              disabled={copying}
            >
              {copying ? (
                <Loader2 className="text-muted-foreground size-4 animate-spin" />
              ) : (
                <ClipboardCopy className="size-4" />
              )}
            </motion.button>
          </div>
        </div>
      </div>

      <AnimatePresence>
        {confirm && (
          <motion.div
            initial={{ opacity: 0 }}
            animate={{ opacity: 1 }}
            exit={{ opacity: 0 }}
            className="bg-background/70 absolute inset-0 z-10 flex flex-col items-center justify-center gap-2 rounded-md backdrop-blur-sm"
          >
            <div className="flex gap-2">
              <Button
                size="sm"
                onClick={() => onRequestDelete(item)}
                variant="destructive"
              >
                Delete
              </Button>
              <Button
                size="sm"
                variant="secondary"
                onClick={() => setConfirm(false)}
              >
                Cancel
              </Button>
            </div>
          </motion.div>
        )}
      </AnimatePresence>
    </motion.div>
  );
};
