import { heicTo, isHeic } from "heic-to";
import { toast } from "sonner";

export async function convertHeicToPng(
  file: File,
  quality = 0.7,
): Promise<File> {
  if (!(await isHeic(file))) return file;
  const res = await heicTo({ blob: file, type: "image/png", quality });
  if (res instanceof Blob)
    return new File([res], file.name.replace(/\.[^.]+$/, ".png"), {
      type: "image/png",
    });
  return file;
}

export async function ensureDirectoryHasNoHeic(
  dir: FileSystemDirectoryHandle,
): Promise<void> {
  let heicCount = 0;
  for await (const [, entry] of dir.entries()) {
    if (entry.kind !== "file") continue;
    const fh = entry as FileSystemFileHandle;
    const f = await fh.getFile();
    if (await isHeic(f)) {
      heicCount++;
    }
  }

  if (heicCount === 0) return;

  let processed = 0;
  const toastId = toast.loading(`Converting HEIC files... (0/${heicCount})`);

  for await (const [name, entry] of dir.entries()) {
    if (entry.kind !== "file") continue;
    const fh = entry as FileSystemFileHandle;
    const f = await fh.getFile();
    if (await isHeic(f)) {
      const png = await convertHeicToPng(f);
      const nh = await dir.getFileHandle(png.name, { create: true });
      const w = await nh.createWritable();
      await w.write(await png.arrayBuffer());
      await w.close();
      await dir.removeEntry(name);

      processed++;
      toast.loading(`Converting HEIC files... (${processed}/${heicCount})`, {
        id: toastId,
      });
    }
  }

  toast.dismiss(toastId);
}
