"use client";

import MediaGrid from "@/components/media-grid";
import { ScrollArea } from "@/components/ui/scroll-area";
import { useSortedMedia } from "@/lib/hooks/use-sorted-media";
import { AnimatePresence, motion } from "framer-motion";
import { useCallback, useEffect, useRef, useState } from "react";

export function MediaScroller() {
  const scrollerRef = useRef<HTMLDivElement>(null);
  const [showTopFade, setShowTopFade] = useState(false);
  const [showBottomFade, setShowBottomFade] = useState(false);
  const { mediaPaths } = useSortedMedia();

  const updateFade = useCallback(() => {
    const el = scrollerRef.current;
    if (!el) return;
    const max = el.scrollHeight - el.clientHeight;
    const atTop = el.scrollTop <= 4;
    const atBottom = el.scrollTop >= max - 4;
    setShowTopFade(max > 0 && !atTop);
    setShowBottomFade(max > 0 && !atBottom);
  }, []);

  useEffect(() => {
    const el = scrollerRef.current;
    if (!el) return;
    updateFade();
    const onScroll = () => updateFade();
    el.addEventListener("scroll", onScroll, { passive: true });
    const ro = new ResizeObserver(() => updateFade());
    ro.observe(el);
    return () => {
      el.removeEventListener("scroll", onScroll);
      ro.disconnect();
    };
  }, [updateFade]);

  useEffect(() => {
    updateFade();
  }, [updateFade, mediaPaths.length]);

  return (
    <div className="relative min-h-0 flex-1">
      <ScrollArea className="h-full p-1 pr-3" viewportRef={scrollerRef}>
        <MediaGrid scrollerRef={scrollerRef} />
      </ScrollArea>
      <AnimatePresence>
        {showTopFade && (
          <motion.div
            key="top-fade"
            initial={{ opacity: 0 }}
            animate={{ opacity: 1 }}
            exit={{ opacity: 0 }}
            className="pointer-events-none absolute top-0 right-3 left-0 z-10 h-12 bg-linear-to-b from-[#151414] to-transparent"
          />
        )}
        {showBottomFade && (
          <motion.div
            key="bottom-fade"
            initial={{ opacity: 0 }}
            animate={{ opacity: 1 }}
            exit={{ opacity: 0 }}
            className="pointer-events-none absolute right-3 bottom-0 left-0 z-10 h-12 bg-linear-to-t from-[#151414] to-transparent"
          />
        )}
      </AnimatePresence>
    </div>
  );
}
