import { AnimatePresence, motion } from "framer-motion";
import { Popover, PopoverContent, PopoverTrigger } from "./ui/popover";
import { Button } from "./ui/button";
import { SendToBack, Trash2, X } from "lucide-react";
import { useGallery } from "@/lib/context/gallery-context";
import { useState } from "react";

export function SelectionMenu() {
  const {
    selection,
    albums,
    clearSelection,
    activeAlbum,
    deleteMedias,
    moveSelectedToAlbum,
  } = useGallery();

  const [deleteConfirmOpen, setDeleteConfirmOpen] = useState(false);

  return (
    <AnimatePresence>
      {selection.size > 0 && (
        <motion.div
          key="multi-action-bar"
          initial={{ opacity: 0, y: -10 }}
          animate={{ opacity: 1, y: 0 }}
          exit={{ opacity: 0, y: -10 }}
          className="border-border fixed top-4 right-4 z-50 flex flex-col gap-2 rounded-lg border bg-black/80 p-4 backdrop-blur-lg"
        >
          <Popover open={deleteConfirmOpen} onOpenChange={setDeleteConfirmOpen}>
            <PopoverTrigger asChild>
              <Button
                variant="ghost"
                className="justify-start text-red-500 hover:bg-red-500/30"
                onClick={() => setDeleteConfirmOpen(true)}
              >
                <Trash2 className="h-4 w-4" /> Delete ({selection.size})
              </Button>
            </PopoverTrigger>
            <PopoverContent className="mr-8 w-64 bg-black/60 backdrop-blur-2xl">
              <p className="mb-2 text-sm">
                Delete {selection.size} selected items?
              </p>
              <Button
                variant="destructive"
                className="mb-2 w-full"
                onClick={() => {
                  deleteMedias(Array.from(selection)).catch(() => undefined);
                  clearSelection();
                  setDeleteConfirmOpen(false);
                }}
              >
                Delete ({selection.size})
              </Button>
              <Button
                variant="secondary"
                className="w-full"
                onClick={() => setDeleteConfirmOpen(false)}
              >
                Cancel
              </Button>
            </PopoverContent>
          </Popover>
          <Popover>
            <PopoverTrigger asChild>
              <Button
                variant="ghost"
                className="justify-start hover:bg-white/10"
              >
                <SendToBack className="h-4 w-4" /> Move to
              </Button>
            </PopoverTrigger>
            <PopoverContent className="max-h-64 w-48 space-y-1 overflow-y-auto bg-black/60 backdrop-blur-2xl">
              {albums
                .filter((a) => a !== activeAlbum)
                .map((a) => (
                  <Button
                    key={a.dirName}
                    variant="ghost"
                    className="w-full justify-start hover:bg-black/15"
                    onClick={() => moveSelectedToAlbum(a)}
                  >
                    {a.name}
                  </Button>
                ))}
            </PopoverContent>
          </Popover>
          <Button
            variant="ghost"
            className="justify-start hover:bg-white/10"
            onClick={clearSelection}
          >
            <X className="h-4 w-4" /> Clear selection
          </Button>
        </motion.div>
      )}
    </AnimatePresence>
  );
}
