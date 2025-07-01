import { MetadataManager } from "@/lib/metadata-manager";
import { isImage, isMedia } from "@/lib/utils";
import type { Album, MediaEntry } from "@/lib/types";
import { ensureDirectoryHasNoHeic } from "./imageService";

async function buildAlbum(
  dir: FileSystemDirectoryHandle,
  uncategorized = false,
): Promise<Album> {
  const meta = await MetadataManager.readDirMeta(dir);
  try {
    await ensureDirectoryHasNoHeic(dir);
  } catch (e) {
    console.log("Failed to ensure directory has no HEIC files:", e);
  }
  const images: string[] = [];
  for await (const [name, h] of dir.entries())
    if (h.kind === "file" && isMedia(name)) images.push(name);

  let thumb: string | null = null;
  try {
    const thumbDir = await dir.getDirectoryHandle(".room237-thumb", {
      create: false,
    });
    for await (const [name, h] of thumbDir.entries())
      if (h.kind === "file" && isImage(name)) {
        const handle = h as FileSystemFileHandle;
        thumb = URL.createObjectURL(await handle.getFile());
        if (thumb) break;
      }
  } catch {}

  return {
    dirName: uncategorized ? "__UNSORTED__" : dir.name,
    name: uncategorized ? "Uncategorized" : (meta.name ?? dir.name),
    images,
    handle: dir,
    thumb,
  } as Album;
}

export async function listAlbums(
  root: FileSystemDirectoryHandle,
): Promise<Album[]> {
  const res: Album[] = [];
  for await (const [, entry] of root.entries())
    if (entry.kind === "directory" && entry.name !== ".room237-thumb")
      res.push(await buildAlbum(entry as FileSystemDirectoryHandle));
  res.unshift(await buildAlbum(root, true));
  return res;
}

export async function createAlbum(
  root: FileSystemDirectoryHandle,
  name: string,
): Promise<Album> {
  const safe = name.trim().replace(/[\/\\:]/g, "_");
  const dir = await root.getDirectoryHandle(safe, { create: true });
  await MetadataManager.writeDirMeta(dir, { name });
  return buildAlbum(dir);
}

export async function deleteAlbum(
  root: FileSystemDirectoryHandle,
  album: Album,
): Promise<void> {
  if (album.dirName === "__UNSORTED__") return;
  await root.removeEntry(album.dirName, { recursive: true });
}

export async function moveMedia(
  source: Album,
  target: Album,
  medias: MediaEntry[],
): Promise<void> {
  for (const media of medias) {
    if (target.images.includes(media.file.name)) continue;
    const fh = await target.handle.getFileHandle(media.file.name, {
      create: true,
    });
    const w = await fh.createWritable();
    await w.write(await media.file.arrayBuffer());
    await w.close();
    await source.handle.removeEntry(media.file.name);
    source.images = source.images.filter((i) => i !== media.file.name);
    target.images.push(media.file.name);
  }
}
