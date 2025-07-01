"use client";

import { useState } from "react";
import type { MediaEntry } from "@/lib/types";

export function useSelection() {
  const [set, setSet] = useState<Set<MediaEntry>>(new Set());
  const toggle = (media: MediaEntry, additive: boolean) => {
    setSet((prev) => {
      const n = new Set(prev);
      if (additive) {
        if (n.has(media)) n.delete(media);
        else n.add(media);
      } else {
        n.clear();
        n.add(media);
      }
      return n;
    });
  };
  const clear = () => setSet(new Set());
  const selectAll = (medias: MediaEntry[]) => setSet(new Set(medias));
  return { selection: set, toggle, clear, selectAll };
}
