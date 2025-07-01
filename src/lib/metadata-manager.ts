import { type FileMeta } from "./types";

export class MetadataManager {
  private static DIR_META = "._meta.json";
  static async readDirMeta(dir: FileSystemDirectoryHandle): Promise<FileMeta> {
    try {
      const metaHandle = await dir.getFileHandle(this.DIR_META);
      const metaFile = await metaHandle.getFile();
      return JSON.parse(await metaFile.text()) as FileMeta;
    } catch {
      return {} as FileMeta;
    }
  }
  static async writeDirMeta(
    dir: FileSystemDirectoryHandle,
    meta: FileMeta,
  ): Promise<void> {
    const fileHandle = await dir.getFileHandle(this.DIR_META, { create: true });
    const writable = await fileHandle.createWritable();
    await writable.write(JSON.stringify(meta));
    await writable.close();
  }
  static async readFileMeta(
    dir: FileSystemDirectoryHandle,
    fileName: string,
  ): Promise<FileMeta> {
    const base = fileName.replace(/\.[^.]+$/, "");
    try {
      const metaHandle = await dir.getFileHandle(`.${base}.json`);
      const metaFile = await metaHandle.getFile();
      return JSON.parse(await metaFile.text()) as FileMeta;
    } catch {
      return {} as FileMeta;
    }
  }
  static async writeFileMeta(
    dir: FileSystemDirectoryHandle,
    fileName: string,
    meta: FileMeta,
  ): Promise<void> {
    const base = fileName.replace(/\.[^.]+$/, "");
    const metaHandle = await dir.getFileHandle(`.${base}.json`, {
      create: true,
    });
    const writable = await metaHandle.createWritable();
    await writable.write(JSON.stringify(meta));
    await writable.close();
  }
}
