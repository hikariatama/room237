export type FileMeta = {
  name?: string;
  added?: number;
  shoot?: number;
  width?: number;
  height?: number;
};

export interface MediaEntry {
  file: File;
  url: () => string;
  unload: () => void;
  thumb: string;
  meta: FileMeta;
  handle: FileSystemFileHandle;
}

export interface Album {
  dirName: string;
  name: string;
  meta: FileMeta;
  thumb: string | null;
  images: string[];
  handle: FileSystemDirectoryHandle;
}

export type LayoutType = "default" | "masonry" | "apple";
