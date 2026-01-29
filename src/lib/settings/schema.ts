import { clamp } from "@/lib/utils";

export const HASH_SCHEMA_VERSION = "1";

export type HashSize = "8x8" | "16x16" | "32x32";
export type HashAlgorithm = "blockhash" | "phash" | "dhash";
export type ResizeFilter = "nearest" | "triangle" | "catmullrom" | "lanczos3";
export type ThreadSetting = number | "auto";

export type DuplicatesSettings = {
  threshold: number;
  hashSize: HashSize;
  hashAlg: HashAlgorithm;
  resizeFilter: ResizeFilter;
  useThumbnailsFirst: boolean;
  maxFilesPerAlbum: number;
};

export type ThumbnailSettings = {
  maxDim: number;
  imageWebpQuality: number;
  imageWebpCompressionLevel: number;
  videoSeekSeconds: number;
  lockPollMs: number;
};

export type FfmpegSettings = {
  threads: ThreadSetting;
  timeoutSecs: number;
  hwaccel: string;
  processWaitPollMs: number;
};

export type PreloadSettings = {
  thumbWorkers: number;
  metaWorkers: number;
  hashWorkers: number;
  progressEmitMs: number;
  thumbHashQueueDelayMs: number;
  thumbHashOnlyAfterIdle: boolean;
  thumbHashRetryOnThumbChange: boolean;
};

export type MetadataSettings = {
  ffmpegProbeTimeoutSecs: number;
  parseCreationTime: boolean;
};

export type AlbumSettings = {
  renameCleanupDelaySecs: number;
  moveRenameThumbsAndMeta: boolean;
};

export type PrivacySettings = {
  enabled: boolean;
  lockscreenEnabled: boolean;
  confirmOpenEnabled: boolean;
};

export type AdvancedSettings = {
  duplicates: DuplicatesSettings;
  thumbnails: ThumbnailSettings;
  ffmpeg: FfmpegSettings;
  preload: PreloadSettings;
  metadata: MetadataSettings;
  album: AlbumSettings;
  privacy: PrivacySettings;
};

export const defaultAdvancedSettings: AdvancedSettings = {
  duplicates: {
    threshold: 32,
    hashSize: "16x16",
    hashAlg: "blockhash",
    resizeFilter: "nearest",
    useThumbnailsFirst: true,
    maxFilesPerAlbum: 0,
  },
  thumbnails: {
    maxDim: 450,
    imageWebpQuality: 75,
    imageWebpCompressionLevel: 3,
    videoSeekSeconds: 1,
    lockPollMs: 50,
  },
  ffmpeg: {
    threads: 4,
    timeoutSecs: 5,
    hwaccel: "auto",
    processWaitPollMs: 50,
  },
  preload: {
    thumbWorkers: 4,
    metaWorkers: 4,
    hashWorkers: 4,
    progressEmitMs: 100,
    thumbHashQueueDelayMs: 10,
    thumbHashOnlyAfterIdle: true,
    thumbHashRetryOnThumbChange: true,
  },
  metadata: {
    ffmpegProbeTimeoutSecs: 5,
    parseCreationTime: true,
  },
  album: {
    renameCleanupDelaySecs: 1,
    moveRenameThumbsAndMeta: true,
  },
  privacy: {
    enabled: false,
    lockscreenEnabled: false,
    confirmOpenEnabled: false,
  },
};

const hashSizeToDimensions: Record<
  HashSize,
  { width: number; height: number }
> = {
  "8x8": { width: 8, height: 8 },
  "16x16": { width: 16, height: 16 },
  "32x32": { width: 32, height: 32 },
};

const clampThreads = (threads: ThreadSetting | undefined): ThreadSetting => {
  if (threads === "auto") return "auto";
  const value = Number.isFinite(threads) ? Number(threads) : undefined;
  if (value === undefined || Number.isNaN(value))
    return defaultAdvancedSettings.ffmpeg.threads;
  return clamp(Math.round(value), 1, 32);
};

const coerceNumber = (value: unknown, fallback: number): number => {
  const n = Number(value);
  return Number.isFinite(n) ? n : fallback;
};

export function clampAdvancedSettings(
  incoming?: Partial<AdvancedSettings>,
): AdvancedSettings {
  const merged: AdvancedSettings = {
    duplicates: {
      ...defaultAdvancedSettings.duplicates,
      ...incoming?.duplicates,
    },
    thumbnails: {
      ...defaultAdvancedSettings.thumbnails,
      ...incoming?.thumbnails,
    },
    ffmpeg: { ...defaultAdvancedSettings.ffmpeg, ...incoming?.ffmpeg },
    preload: { ...defaultAdvancedSettings.preload, ...incoming?.preload },
    metadata: { ...defaultAdvancedSettings.metadata, ...incoming?.metadata },
    album: { ...defaultAdvancedSettings.album, ...incoming?.album },
    privacy: { ...defaultAdvancedSettings.privacy, ...incoming?.privacy },
  };

  merged.duplicates.threshold = clamp(
    coerceNumber(
      merged.duplicates.threshold,
      defaultAdvancedSettings.duplicates.threshold,
    ),
    0,
    128,
  );
  merged.duplicates.maxFilesPerAlbum = clamp(
    coerceNumber(
      merged.duplicates.maxFilesPerAlbum,
      defaultAdvancedSettings.duplicates.maxFilesPerAlbum,
    ),
    0,
    20_000,
  );
  if (!hashSizeToDimensions[merged.duplicates.hashSize]) {
    merged.duplicates.hashSize = defaultAdvancedSettings.duplicates.hashSize;
  }
  if (!["blockhash", "phash", "dhash"].includes(merged.duplicates.hashAlg)) {
    merged.duplicates.hashAlg = defaultAdvancedSettings.duplicates.hashAlg;
  }
  if (
    !["nearest", "triangle", "catmullrom", "lanczos3"].includes(
      merged.duplicates.resizeFilter,
    )
  ) {
    merged.duplicates.resizeFilter =
      defaultAdvancedSettings.duplicates.resizeFilter;
  }

  merged.thumbnails.maxDim = clamp(
    coerceNumber(
      merged.thumbnails.maxDim,
      defaultAdvancedSettings.thumbnails.maxDim,
    ),
    128,
    2048,
  );
  merged.thumbnails.imageWebpQuality = clamp(
    coerceNumber(
      merged.thumbnails.imageWebpQuality,
      defaultAdvancedSettings.thumbnails.imageWebpQuality,
    ),
    30,
    95,
  );
  merged.thumbnails.imageWebpCompressionLevel = clamp(
    coerceNumber(
      merged.thumbnails.imageWebpCompressionLevel,
      defaultAdvancedSettings.thumbnails.imageWebpCompressionLevel,
    ),
    0,
    9,
  );
  merged.thumbnails.videoSeekSeconds = clamp(
    coerceNumber(
      merged.thumbnails.videoSeekSeconds,
      defaultAdvancedSettings.thumbnails.videoSeekSeconds,
    ),
    0,
    30,
  );
  merged.thumbnails.lockPollMs = clamp(
    coerceNumber(
      merged.thumbnails.lockPollMs,
      defaultAdvancedSettings.thumbnails.lockPollMs,
    ),
    5,
    250,
  );

  merged.ffmpeg.threads = clampThreads(merged.ffmpeg.threads);
  merged.ffmpeg.timeoutSecs = clamp(
    coerceNumber(
      merged.ffmpeg.timeoutSecs,
      defaultAdvancedSettings.ffmpeg.timeoutSecs,
    ),
    1,
    60,
  );
  merged.ffmpeg.processWaitPollMs = clamp(
    coerceNumber(
      merged.ffmpeg.processWaitPollMs,
      defaultAdvancedSettings.ffmpeg.processWaitPollMs,
    ),
    5,
    200,
  );
  merged.ffmpeg.hwaccel =
    merged.ffmpeg.hwaccel?.trim() || defaultAdvancedSettings.ffmpeg.hwaccel;

  merged.preload.thumbWorkers = clamp(
    coerceNumber(
      merged.preload.thumbWorkers,
      defaultAdvancedSettings.preload.thumbWorkers,
    ),
    1,
    32,
  );
  merged.preload.metaWorkers = clamp(
    coerceNumber(
      merged.preload.metaWorkers,
      defaultAdvancedSettings.preload.metaWorkers,
    ),
    1,
    32,
  );
  merged.preload.hashWorkers = clamp(
    coerceNumber(
      merged.preload.hashWorkers,
      defaultAdvancedSettings.preload.hashWorkers,
    ),
    1,
    32,
  );
  merged.preload.progressEmitMs = clamp(
    coerceNumber(
      merged.preload.progressEmitMs,
      defaultAdvancedSettings.preload.progressEmitMs,
    ),
    50,
    1000,
  );
  merged.preload.thumbHashQueueDelayMs = clamp(
    coerceNumber(
      merged.preload.thumbHashQueueDelayMs,
      defaultAdvancedSettings.preload.thumbHashQueueDelayMs,
    ),
    0,
    100,
  );

  merged.metadata.ffmpegProbeTimeoutSecs = clamp(
    coerceNumber(
      merged.metadata.ffmpegProbeTimeoutSecs,
      merged.ffmpeg.timeoutSecs ?? defaultAdvancedSettings.ffmpeg.timeoutSecs,
    ),
    1,
    60,
  );

  merged.album.renameCleanupDelaySecs = clamp(
    coerceNumber(
      merged.album.renameCleanupDelaySecs,
      defaultAdvancedSettings.album.renameCleanupDelaySecs,
    ),
    0,
    10,
  );

  merged.privacy.enabled = Boolean(merged.privacy.enabled);
  if (merged.privacy.enabled) {
    merged.privacy.lockscreenEnabled = true;
    merged.privacy.confirmOpenEnabled = true;
  } else {
    merged.privacy.lockscreenEnabled = Boolean(
      merged.privacy.lockscreenEnabled,
    );
    merged.privacy.confirmOpenEnabled = Boolean(
      merged.privacy.confirmOpenEnabled,
    );
  }

  return merged;
}

export function getHashDimensions(size: HashSize): {
  width: number;
  height: number;
  bits: number;
} {
  const dims =
    hashSizeToDimensions[size] ??
    hashSizeToDimensions[defaultAdvancedSettings.duplicates.hashSize];
  return { ...dims, bits: dims.width * dims.height };
}

export function effectiveThreshold(settings: AdvancedSettings): number {
  const { bits } = getHashDimensions(settings.duplicates.hashSize);
  return Math.round(settings.duplicates.threshold * (bits / 256));
}

export function deriveHashVersionSeed(settings: AdvancedSettings): string {
  const dims = getHashDimensions(settings.duplicates.hashSize);
  const thumbSeed = [
    settings.thumbnails.maxDim,
    settings.thumbnails.imageWebpQuality,
    settings.thumbnails.imageWebpCompressionLevel,
    settings.thumbnails.videoSeekSeconds.toFixed(3),
  ].join("|");
  return [
    HASH_SCHEMA_VERSION,
    settings.duplicates.hashAlg,
    `${dims.width}x${dims.height}`,
    settings.duplicates.resizeFilter,
    settings.duplicates.useThumbnailsFirst ? "thumb-first" : "original-first",
    thumbSeed,
  ].join("|");
}
