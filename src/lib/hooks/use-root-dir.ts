"use client";

import { useState } from "react";

export function useRootDir() {
  const [root, setRoot] = useState<FileSystemDirectoryHandle | null>(null);
  const pickDirectory = async () => {
    const dir = await window.showDirectoryPicker();
    setRoot(dir);
  };
  return { rootDir: root, pickDirectory };
}
