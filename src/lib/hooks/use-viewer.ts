"use client";

import { useState } from "react";

export function useViewer(total: number) {
  const [index, setIndex] = useState<number | null>(null);
  const open = (i: number) => setIndex(i);
  const close = () => setIndex(null);
  const next = () => setIndex((i) => (i === null ? null : (i + 1) % total));
  const prev = () =>
    setIndex((i) => (i === null ? null : (i - 1 + total) % total));
  return { viewerIndex: index, open, close, next, prev };
}
