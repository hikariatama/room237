"use client";

import { ScrollArea } from "@/components/ui/scroll-area";
import { AlbumItem } from "@/components/album-item";
import { useGallery } from "@/lib/context/gallery-context";
import { NewAlbumButton } from "@/components/sidebar/new-album-button";

export default function AlbumList() {
  const { albums, activeAlbum, setActive, rootDir } = useGallery();
  return (
    <ScrollArea className="h-[calc(100vh-6rem)] space-y-2 p-2">
      {albums.map((a) => (
        <AlbumItem
          key={a.dirName}
          album={a}
          active={a.dirName === activeAlbum?.dirName}
          onClick={() => setActive(a.dirName)}
        />
      ))}
      {rootDir && <NewAlbumButton />}
    </ScrollArea>
  );
}
