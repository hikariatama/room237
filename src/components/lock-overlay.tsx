"use client";

import { AnimatePresence, motion } from "framer-motion";
import { EyeOff } from "lucide-react";

export function LockOverlay({ locked }: { locked: boolean }) {
  return (
    <AnimatePresence>
      {locked && (
        <motion.div
          key="lockscreen"
          initial={{ opacity: 0 }}
          animate={{ opacity: 1, transition: { duration: 0.05 } }}
          exit={{ opacity: 0, transition: { duration: 0.3 } }}
          className="fixed inset-0 z-[9999] flex flex-col items-center justify-center gap-4 bg-black/70 pb-6 backdrop-blur-2xl"
        >
          <EyeOff className="h-14 w-14" />
          <span className="text-2xl font-medium">Sensitive content</span>
          <span className="text-muted-foreground max-w-sm text-center text-base">
            This website contains sensitive content which some people may find
            offensive or disturbing.
          </span>
        </motion.div>
      )}
    </AnimatePresence>
  );
}
