"use client";

import { Button } from "@/components/ui/button";
import {
  Popover,
  PopoverContent,
  PopoverTrigger,
} from "@/components/ui/popover";
import { Slider } from "@/components/ui/slider";
import { useGallery, type SortKey } from "@/lib/context/gallery-context";
import { AnimatePresence, motion } from "framer-motion";
import { ArrowDownAZ, ArrowUpAZ, Trash2 } from "lucide-react";
import { DynamicIcon, type IconName } from "lucide-react/dynamic";
import { useState } from "react";
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from "./select";

const SORT_KEYS: Record<SortKey, { title: string; icon: IconName }> = {
  shoot: {
    title: "EXIF Date",
    icon: "camera",
  },
  added: {
    title: "Added Date",
    icon: "calendar",
  },
  name: {
    title: "Name",
    icon: "file-text",
  },
};

export default function MediaGridHeader() {
  const {
    columns,
    setColumns,
    sortKey,
    setSortKey,
    sortDir,
    setSortDir,
    activeAlbum,
    deleteAlbum,
    rootDir,
  } = useGallery();

  const [deleteOpen, setDeleteOpen] = useState(false);

  if (!rootDir) {
    return null;
  }

  return (
    <div className="mb-3 flex items-center justify-between">
      <div className="flex items-center gap-2">
        <Select
          value={sortKey}
          onValueChange={(value) => setSortKey(value as SortKey)}
        >
          <SelectTrigger className="cursor-pointer">
            <SelectValue />
          </SelectTrigger>
          <SelectContent>
            {(["shoot", "added", "name"] as const).map((key) => (
              <SelectItem key={key} value={key} className="cursor-pointer">
                <DynamicIcon name={SORT_KEYS[key].icon} />
                {SORT_KEYS[key].title}
              </SelectItem>
            ))}
          </SelectContent>
        </Select>
        <Button
          variant="outline"
          onClick={() => setSortDir(sortDir === "asc" ? "desc" : "asc")}
        >
          {sortDir === "asc" ? <ArrowDownAZ /> : <ArrowUpAZ />}
        </Button>
        <div className="w-40">
          <Slider
            value={[columns]}
            min={2}
            max={8}
            step={1}
            onValueChange={(v) => {
              if (v[0]) setColumns(v[0]);
            }}
          />
        </div>
      </div>
      <AnimatePresence>
        {activeAlbum?.dirName !== "__UNSORTED__" && (
          <motion.div
            key="delete-album-button"
            initial={{ opacity: 0 }}
            animate={{ opacity: 1 }}
            exit={{ opacity: 0 }}
            transition={{ duration: 0.1 }}
          >
            <Popover open={deleteOpen} onOpenChange={setDeleteOpen}>
              <PopoverTrigger asChild>
                <Button
                  variant="destructive"
                  onClick={() => setDeleteOpen(true)}
                >
                  <Trash2 />
                  Delete album
                </Button>
              </PopoverTrigger>
              <PopoverContent>
                <p className="text-muted-foreground mb-4 text-sm">
                  Are you sure you want to delete the album{" "}
                  <span className="font-semibold">{activeAlbum?.name}</span>?
                  <br />
                  This action cannot be undone.
                </p>
                <div className="flex gap-2">
                  <Button
                    variant="destructive"
                    onClick={() => activeAlbum && deleteAlbum(activeAlbum)}
                    className="flex-auto"
                  >
                    Confirm
                  </Button>
                  <Button
                    variant="secondary"
                    onClick={() => setDeleteOpen(false)}
                  >
                    Cancel
                  </Button>
                </div>
              </PopoverContent>
            </Popover>
          </motion.div>
        )}
      </AnimatePresence>
    </div>
  );
}
