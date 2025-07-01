"use client";

import { useState } from "react";

export function useLockscreen() {
  const [locked, setLocked] = useState(false);
  const lock = () => setLocked(true);
  const unlock = () => setLocked(false);
  return { locked, lock, unlock };
}
