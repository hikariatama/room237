"use client";

import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import {
  Popover,
  PopoverContent,
  PopoverTrigger,
} from "@/components/ui/popover";
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
} from "@/components/ui/select";
import { Slider } from "@/components/ui/slider";
import {
  defaultAdvancedSettings,
  type AdvancedSettings,
} from "@/lib/settings/schema";
import { useAdvancedSettings } from "@/lib/settings/store";
import { useRoom237 } from "@/lib/stores";
import { IconLoader2 } from "@tabler/icons-react";
import { useEffect, useState, type ReactNode } from "react";
import { useI18n } from "@/lib/i18n";
import { toast } from "./toaster";
import { openUrl } from "@tauri-apps/plugin-opener";

function Section({
  title,
  onReset,
  resetLabel,
  children,
}: {
  title: string;
  onReset?: () => void;
  resetLabel?: string;
  children: ReactNode;
}) {
  return (
    <div className="border-border bg-muted/20 mb-4 rounded-lg border p-3">
      <div className="mb-3 flex items-center justify-between gap-3">
        <div className="font-semibold">{title}</div>
        {onReset && (
          <Button size="sm" variant="ghost" onClick={onReset}>
            {resetLabel ?? "Reset section"}
          </Button>
        )}
      </div>
      <div className="flex flex-col gap-3">{children}</div>
    </div>
  );
}

function Field({
  label,
  helper,
  default: defaultValue,
  children,
}: {
  label: string;
  helper?: string;
  default?: string;
  children: ReactNode;
}) {
  return (
    <div className="mb-2 flex flex-col gap-1.5">
      <div className="flex items-center gap-2">
        <div className="text-sm font-medium">{label}</div>
        {defaultValue && (
          <div className="text-muted-foreground border-border rounded-full border px-1 text-xs">
            default: {defaultValue}
          </div>
        )}
      </div>
      {helper ? (
        <div className="text-muted-foreground text-xs">{helper}</div>
      ) : null}
      {children}
    </div>
  );
}

export function AdvancedSettingsPopover({
  side = "right",
  align = "start",
  trigger,
}: {
  side?: "left" | "right";
  align?: "start" | "center" | "end";
  trigger?: ReactNode;
}) {
  const { settings, loading, initialized, updateField, refresh, save, reset } =
    useAdvancedSettings();
  const setAllowOpen = useRoom237((state) => state.setAllowOpen);
  const { t } = useI18n();
  const [open, setOpen] = useState(false);

  useEffect(() => {
    if (open && !initialized) {
      void refresh();
    }
  }, [open, initialized, refresh]);

  const resetSection = (section: keyof AdvancedSettings) =>
    updateField([section], defaultAdvancedSettings[section]);

  const onSave = async () => {
    try {
      await save();
      setAllowOpen(true);
      toast.success(t("advanced.saveSuccess"));
      setOpen(false);
    } catch (error) {
      toast.error((error as Error).message ?? t("advanced.saveError"));
    }
  };

  const onResetAll = async () => {
    await reset();
    toast.success(t("advanced.resetDefaults"));
  };

  const privacyEnabled = settings.privacy.enabled;
  const lockscreenActive = privacyEnabled || settings.privacy.lockscreenEnabled;
  const confirmOpenActive =
    privacyEnabled || settings.privacy.confirmOpenEnabled;

  return (
    <Popover open={open} onOpenChange={setOpen}>
      <PopoverTrigger asChild>
        {trigger ?? (
          <Button variant="outline" className="w-full">
            {t("advanced.trigger")}
          </Button>
        )}
      </PopoverTrigger>
      <PopoverContent
        side={side}
        align={align}
        className="z-50 max-h-[80vh] w-120 max-w-[90vw] overflow-y-auto p-4 shadow-xl"
      >
        <div className="mb-4">
          <div className="text-lg font-semibold">{t("advanced.title")}</div>
          <div className="text-xs text-red-500">{t("advanced.disclaimer")}</div>
        </div>

        <Section
          title={t("advanced.section.privacy")}
          onReset={() => resetSection("privacy")}
          resetLabel={t("advanced.resetSection")}
        >
          <Field
            label={t("advanced.field.privacyFeatures")}
            helper={t("advanced.field.privacyFeatures.helper")}
            default={t("common.off")}
          >
            <Button
              size="sm"
              variant={privacyEnabled ? "default" : "outline"}
              onClick={() =>
                updateField(["privacy", "enabled"], !settings.privacy.enabled)
              }
              className="w-fit"
            >
              {privacyEnabled ? t("common.on") : t("common.off")}
            </Button>
          </Field>
          <Field
            label={t("advanced.field.lockscreen")}
            helper={t("advanced.field.lockscreen.helper")}
            default={t("common.off")}
          >
            <Button
              size="sm"
              variant={lockscreenActive ? "default" : "outline"}
              onClick={() =>
                updateField(
                  ["privacy", "lockscreenEnabled"],
                  !settings.privacy.lockscreenEnabled,
                )
              }
              className="w-fit"
              disabled={privacyEnabled}
            >
              {privacyEnabled || lockscreenActive
                ? t("common.on")
                : t("common.off")}
            </Button>
          </Field>
          <Field
            label={t("advanced.field.confirmOpen")}
            helper={t("advanced.field.confirmOpen.helper")}
            default={t("common.off")}
          >
            <Button
              size="sm"
              variant={confirmOpenActive ? "default" : "outline"}
              onClick={() =>
                updateField(
                  ["privacy", "confirmOpenEnabled"],
                  !settings.privacy.confirmOpenEnabled,
                )
              }
              className="w-fit"
              disabled={privacyEnabled}
            >
              {privacyEnabled || confirmOpenActive
                ? t("common.on")
                : t("common.off")}
            </Button>
          </Field>
        </Section>

        <Section
          title={t("advanced.section.duplicates")}
          onReset={() => resetSection("duplicates")}
          resetLabel={t("advanced.resetSection")}
        >
          <Field
            label={t("advanced.field.duplicates.threshold")}
            helper={t("advanced.field.duplicates.threshold.helper")}
            default="32"
          >
            <Slider
              value={[settings.duplicates.threshold]}
              min={0}
              max={64}
              step={1}
              onValueChange={(v) =>
                updateField(["duplicates", "threshold"], v[0] ?? 0)
              }
              className="flex-1"
            />
          </Field>
          <Field
            label={t("advanced.field.duplicates.hashSize")}
            helper={t("advanced.field.duplicates.hashSize.helper")}
          >
            <Select
              value={settings.duplicates.hashSize}
              onValueChange={(v) => updateField(["duplicates", "hashSize"], v)}
            >
              <SelectTrigger>
                {
                  {
                    "8x8": "8 x 8",
                    "16x16": "16 x 16",
                    "32x32": "32 x 32",
                  }[settings.duplicates.hashSize]
                }
              </SelectTrigger>
              <SelectContent>
                <SelectItem value="8x8">8 x 8</SelectItem>
                <SelectItem value="16x16">
                  <div className="flex items-center gap-2">
                    <div>16 x 16</div>
                    <div className="text-muted-foreground border-border rounded-full border px-1 text-xs">
                      default
                    </div>
                  </div>
                </SelectItem>
                <SelectItem value="32x32">32 x 32</SelectItem>
              </SelectContent>
            </Select>
          </Field>
          <Field
            label={t("advanced.field.duplicates.algorithm")}
            helper={t("advanced.field.duplicates.algorithm.helper")}
          >
            <Select
              value={settings.duplicates.hashAlg}
              onValueChange={(v) => updateField(["duplicates", "hashAlg"], v)}
            >
              <SelectTrigger>
                {
                  {
                    blockhash: "Blockhash",
                    phash: "Perceptual",
                    dhash: "dHash",
                  }[settings.duplicates.hashAlg]
                }
              </SelectTrigger>
              <SelectContent>
                <SelectItem value="blockhash">
                  <div className="flex max-w-64 flex-col items-start gap-1">
                    <div className="flex items-center gap-2">
                      <div>Blockhash</div>
                      <div className="text-muted-foreground border-border rounded-full border px-1 text-xs">
                        default
                      </div>
                    </div>
                    <div className="text-muted-foreground text-xs">
                      Faster. Good general-purpose choice; typically less
                      accurate than perceptual.
                    </div>
                  </div>
                </SelectItem>
                <SelectItem value="phash">
                  <div className="flex max-w-64 flex-col items-start gap-1">
                    <div>Perceptual</div>
                    <div className="text-muted-foreground text-xs">
                      Slower. Focuses on overall image structure. Often more
                      accurate for general images.
                    </div>
                  </div>
                </SelectItem>
                <SelectItem value="dhash">
                  <div className="flex max-w-64 flex-col items-start gap-1">
                    <div>dHash</div>
                    <div className="text-muted-foreground text-xs">
                      Moderate speed. Captures gradient changes. Good for images
                      with distinct edges.
                    </div>
                  </div>
                </SelectItem>
              </SelectContent>
            </Select>
          </Field>
          <Field
            label={t("advanced.field.duplicates.resizeFilter")}
            helper={t("advanced.field.duplicates.resizeFilter.helper")}
          >
            <Select
              value={settings.duplicates.resizeFilter}
              onValueChange={(v) =>
                updateField(["duplicates", "resizeFilter"], v)
              }
            >
              <SelectTrigger>
                {
                  {
                    nearest: "Nearest",
                    triangle: "Triangle",
                    catmullrom: "Catmull-Rom",
                    lanczos3: "Lanczos3",
                  }[settings.duplicates.resizeFilter]
                }
              </SelectTrigger>
              <SelectContent>
                <SelectItem value="nearest">
                  <div className="flex max-w-64 flex-col items-start gap-1">
                    <div className="flex items-center gap-2">
                      <div>Nearest</div>
                      <div className="text-muted-foreground border-border rounded-full border px-1 text-xs">
                        default
                      </div>
                    </div>
                    <div className="text-muted-foreground text-xs">
                      Fastest but lowest quality. May produce pixelated images.
                    </div>
                  </div>
                </SelectItem>
                <SelectItem value="triangle">
                  <div className="flex max-w-64 flex-col items-start gap-1">
                    <div>Triangle</div>
                    <div className="text-muted-foreground text-xs">
                      Balanced quality and speed. Good for general resizing
                      tasks.
                    </div>
                  </div>
                </SelectItem>
                <SelectItem value="catmullrom">
                  <div className="flex max-w-64 flex-col items-start gap-1">
                    <div>Catmull-Rom</div>
                    <div className="text-muted-foreground text-xs">
                      Higher quality with moderate speed. Preserves details
                      well.
                    </div>
                  </div>
                </SelectItem>
                <SelectItem value="lanczos3">
                  <div className="flex max-w-64 flex-col items-start gap-1">
                    <div>Lanczos3</div>
                    <div className="text-muted-foreground text-xs">
                      Best quality but slowest. Ideal for high-quality image
                      processing.
                    </div>
                  </div>
                </SelectItem>
              </SelectContent>
            </Select>
          </Field>
          <Field
            label={t("advanced.field.duplicates.useThumbsFirst")}
            helper={t("advanced.field.duplicates.useThumbsFirst.helper")}
            default={t("common.enabled")}
          >
            <Button
              size="sm"
              variant={
                settings.duplicates.useThumbnailsFirst ? "default" : "outline"
              }
              onClick={() =>
                updateField(
                  ["duplicates", "useThumbnailsFirst"],
                  !settings.duplicates.useThumbnailsFirst,
                )
              }
              className="w-fit"
            >
              {settings.duplicates.useThumbnailsFirst
                ? t("common.enabled")
                : t("common.disabled")}
            </Button>
          </Field>
          <Field
            label={t("advanced.field.duplicates.maxPerAlbum")}
            helper={t("advanced.field.duplicates.maxPerAlbum.helper")}
            default="0"
          >
            <Input
              type="number"
              value={settings.duplicates.maxFilesPerAlbum}
              onChange={(e) =>
                updateField(
                  ["duplicates", "maxFilesPerAlbum"],
                  Number(e.target.value),
                )
              }
            />
          </Field>
        </Section>

        <Section
          title={t("advanced.section.thumbnails")}
          onReset={() => resetSection("thumbnails")}
          resetLabel={t("advanced.resetSection")}
        >
          <Field
            label={t("advanced.field.thumbs.maxDim")}
            helper={t("advanced.field.thumbs.maxDim.helper")}
            default="450"
          >
            <Input
              type="number"
              value={settings.thumbnails.maxDim}
              onChange={(e) =>
                updateField(["thumbnails", "maxDim"], Number(e.target.value))
              }
            />
          </Field>
          <Field
            label={t("advanced.field.thumbs.webpQuality")}
            helper={t("advanced.field.thumbs.webpQuality.helper")}
            default="75"
          >
            <Slider
              min={30}
              max={95}
              step={1}
              value={[settings.thumbnails.imageWebpQuality]}
              onValueChange={(v) =>
                updateField(["thumbnails", "imageWebpQuality"], v[0] ?? 75)
              }
            />
          </Field>
          <Field
            label={t("advanced.field.thumbs.webpCompression")}
            helper={t("advanced.field.thumbs.webpCompression.helper")}
            default="3"
          >
            <Slider
              min={0}
              max={9}
              step={1}
              value={[settings.thumbnails.imageWebpCompressionLevel]}
              onValueChange={(v) =>
                updateField(
                  ["thumbnails", "imageWebpCompressionLevel"],
                  v[0] ?? 3,
                )
              }
            />
          </Field>
          <Field
            label={t("advanced.field.thumbs.videoSeek")}
            helper={t("advanced.field.thumbs.videoSeek.helper")}
            default="1"
          >
            <Input
              type="number"
              step="0.1"
              value={settings.thumbnails.videoSeekSeconds}
              onChange={(e) =>
                updateField(
                  ["thumbnails", "videoSeekSeconds"],
                  Number(e.target.value),
                )
              }
            />
          </Field>
          <Field
            label={t("advanced.field.thumbs.lockPoll")}
            helper={t("advanced.field.thumbs.lockPoll.helper")}
            default="50"
          >
            <Input
              type="number"
              value={settings.thumbnails.lockPollMs}
              onChange={(e) =>
                updateField(
                  ["thumbnails", "lockPollMs"],
                  Number(e.target.value),
                )
              }
            />
          </Field>
        </Section>

        <Section
          title={t("advanced.section.ffmpeg")}
          onReset={() => resetSection("ffmpeg")}
          resetLabel={t("advanced.resetSection")}
        >
          <Field
            label={t("advanced.field.ffmpeg.threads")}
            helper={t("advanced.field.ffmpeg.threads.helper")}
            default="auto"
          >
            <div className="flex flex-col gap-1.5">
              <Slider
                min={0}
                max={32}
                step={1}
                value={[
                  settings.ffmpeg.threads === "auto"
                    ? 0
                    : typeof settings.ffmpeg.threads === "number"
                      ? settings.ffmpeg.threads
                      : 4,
                ]}
                onValueChange={(v) => {
                  const n = v[0] ?? 0;
                  updateField(["ffmpeg", "threads"], n === 0 ? "auto" : n);
                }}
                className="flex-1"
              />
              <div className="text-muted-foreground text-xs">
                {settings.ffmpeg.threads === "auto"
                  ? "Auto"
                  : String(settings.ffmpeg.threads)}
              </div>
            </div>
          </Field>
          <Field
            label={t("advanced.field.ffmpeg.timeout")}
            helper={t("advanced.field.ffmpeg.timeout.helper")}
            default="5"
          >
            <Input
              type="number"
              value={settings.ffmpeg.timeoutSecs}
              onChange={(e) =>
                updateField(["ffmpeg", "timeoutSecs"], Number(e.target.value))
              }
            />
          </Field>
          <Field
            label={t("advanced.field.ffmpeg.hwaccel")}
            helper={t("advanced.field.ffmpeg.hwaccel.helper")}
            default="auto"
          >
            <Input
              type="text"
              value={settings.ffmpeg.hwaccel}
              onChange={(e) =>
                updateField(["ffmpeg", "hwaccel"], e.target.value || "auto")
              }
            />
          </Field>
          <Field
            label={t("advanced.field.ffmpeg.processPoll")}
            helper={t("advanced.field.ffmpeg.processPoll.helper")}
            default="50"
          >
            <Input
              type="number"
              value={settings.ffmpeg.processWaitPollMs}
              onChange={(e) =>
                updateField(
                  ["ffmpeg", "processWaitPollMs"],
                  Number(e.target.value),
                )
              }
            />
          </Field>
        </Section>

        <Section
          title={t("advanced.section.preload")}
          onReset={() => resetSection("preload")}
          resetLabel={t("advanced.resetSection")}
        >
          <Field
            label={t("advanced.field.preload.thumbWorkers")}
            helper={t("advanced.field.preload.thumbWorkers.helper")}
            default="4"
          >
            <div className="flex items-center gap-2">
              <Slider
                min={1}
                max={32}
                step={1}
                value={[settings.preload.thumbWorkers]}
                onValueChange={(v) =>
                  updateField(["preload", "thumbWorkers"], v[0] ?? 4)
                }
                className="flex-1"
              />
              <span className="text-muted-foreground w-6 text-right text-xs tabular-nums">
                {settings.preload.thumbWorkers}
              </span>
            </div>
          </Field>
          <Field
            label={t("advanced.field.preload.metaWorkers")}
            helper={t("advanced.field.preload.metaWorkers.helper")}
            default="4"
          >
            <div className="flex items-center gap-2">
              <Slider
                min={1}
                max={32}
                step={1}
                value={[settings.preload.metaWorkers]}
                onValueChange={(v) =>
                  updateField(["preload", "metaWorkers"], v[0] ?? 4)
                }
                className="flex-1"
              />
              <span className="text-muted-foreground w-6 text-right text-xs tabular-nums">
                {settings.preload.metaWorkers}
              </span>
            </div>
          </Field>
          <Field
            label={t("advanced.field.preload.hashWorkers")}
            helper={t("advanced.field.preload.hashWorkers.helper")}
            default="4"
          >
            <div className="flex items-center gap-2">
              <Slider
                min={1}
                max={32}
                step={1}
                value={[settings.preload.hashWorkers]}
                onValueChange={(v) =>
                  updateField(["preload", "hashWorkers"], v[0] ?? 4)
                }
                className="flex-1"
              />
              <span className="text-muted-foreground w-6 text-right text-xs tabular-nums">
                {settings.preload.hashWorkers}
              </span>
            </div>
          </Field>
          <Field
            label={t("advanced.field.preload.progressEmit")}
            helper={t("advanced.field.preload.progressEmit.helper")}
            default="100"
          >
            <Input
              type="number"
              value={settings.preload.progressEmitMs}
              onChange={(e) =>
                updateField(
                  ["preload", "progressEmitMs"],
                  Number(e.target.value),
                )
              }
            />
          </Field>
          <Field
            label={t("advanced.field.preload.hashDelay")}
            helper={t("advanced.field.preload.hashDelay.helper")}
            default="10"
          >
            <Input
              type="number"
              value={settings.preload.thumbHashQueueDelayMs}
              onChange={(e) =>
                updateField(
                  ["preload", "thumbHashQueueDelayMs"],
                  Number(e.target.value),
                )
              }
            />
          </Field>
          <Field
            label={t("advanced.field.preload.hashAfter")}
            helper={t("advanced.field.preload.hashAfter.helper")}
            default={t("common.yes")}
          >
            <Button
              size="sm"
              variant={
                settings.preload.thumbHashOnlyAfterIdle ? "default" : "outline"
              }
              onClick={() =>
                updateField(
                  ["preload", "thumbHashOnlyAfterIdle"],
                  !settings.preload.thumbHashOnlyAfterIdle,
                )
              }
              className="w-fit"
            >
              {settings.preload.thumbHashOnlyAfterIdle
                ? t("common.yes")
                : t("common.no")}
            </Button>
          </Field>
          <Field
            label={t("advanced.field.preload.hashRetry")}
            helper={t("advanced.field.preload.hashRetry.helper")}
            default={t("common.enabled")}
          >
            <Button
              size="sm"
              variant={
                settings.preload.thumbHashRetryOnThumbChange
                  ? "default"
                  : "outline"
              }
              onClick={() =>
                updateField(
                  ["preload", "thumbHashRetryOnThumbChange"],
                  !settings.preload.thumbHashRetryOnThumbChange,
                )
              }
              className="w-fit"
            >
              {settings.preload.thumbHashRetryOnThumbChange
                ? t("common.enabled")
                : t("common.off")}
            </Button>
          </Field>
        </Section>

        <Section
          title={t("advanced.section.metadata")}
          onReset={() => resetSection("metadata")}
          resetLabel={t("advanced.resetSection")}
        >
          <Field
            label={t("advanced.field.metadata.probeTimeout")}
            helper={t("advanced.field.metadata.probeTimeout.helper")}
            default="5"
          >
            <Input
              type="number"
              value={settings.metadata.ffmpegProbeTimeoutSecs}
              onChange={(e) =>
                updateField(
                  ["metadata", "ffmpegProbeTimeoutSecs"],
                  Number(e.target.value),
                )
              }
            />
          </Field>
          <Field
            label={t("advanced.field.metadata.parseCreation")}
            helper={t("advanced.field.metadata.parseCreation.helper")}
            default={t("common.enabled")}
          >
            <Button
              size="sm"
              variant={
                settings.metadata.parseCreationTime ? "default" : "outline"
              }
              onClick={() =>
                updateField(
                  ["metadata", "parseCreationTime"],
                  !settings.metadata.parseCreationTime,
                )
              }
              className="w-fit"
            >
              {settings.metadata.parseCreationTime
                ? t("common.enabled")
                : t("common.disabled")}
            </Button>
          </Field>
        </Section>

        <Section
          title={t("advanced.section.album")}
          onReset={() => resetSection("album")}
          resetLabel={t("advanced.resetSection")}
        >
          <Field
            label={t("advanced.field.album.renameDelay")}
            helper={t("advanced.field.album.renameDelay.helper")}
            default="1"
          >
            <Input
              type="number"
              value={settings.album.renameCleanupDelaySecs}
              onChange={(e) =>
                updateField(
                  ["album", "renameCleanupDelaySecs"],
                  Number(e.target.value),
                )
              }
            />
          </Field>
          <Field
            label={t("advanced.field.album.moveThumbs")}
            helper={t("advanced.field.album.moveThumbs.helper")}
            default={t("common.enabled")}
          >
            <Button
              size="sm"
              variant={
                settings.album.moveRenameThumbsAndMeta ? "default" : "outline"
              }
              onClick={() =>
                updateField(
                  ["album", "moveRenameThumbsAndMeta"],
                  !settings.album.moveRenameThumbsAndMeta,
                )
              }
              className="w-fit"
            >
              {settings.album.moveRenameThumbsAndMeta
                ? t("common.enabled")
                : t("common.disabled")}
            </Button>
          </Field>
        </Section>

        <div className="text-muted-foreground mt-3 flex items-center gap-1.5 text-xs">
          <svg
            width="24"
            height="24"
            viewBox="0 0 24 24"
            fill="none"
            xmlns="http://www.w3.org/2000/svg"
            className="size-4"
          >
            <mask
              id="yirek"
              style={{ maskType: "alpha" }}
              maskUnits="userSpaceOnUse"
              x="2"
              y="3"
              width="20"
              height="18"
            >
              <path
                d="M2 9.49998C2.00002 8.38718 2.33759 7.30056 2.96813 6.38364C3.59867 5.46672 4.49252 4.76264 5.53161 4.36438C6.5707 3.96612 7.70616 3.89242 8.78801 4.15302C9.86987 4.41362 10.8472 4.99626 11.591 5.82398C11.6434 5.87999 11.7067 5.92465 11.7771 5.95518C11.8474 5.98571 11.9233 6.00146 12 6.00146C12.0767 6.00146 12.1526 5.98571 12.2229 5.95518C12.2933 5.92465 12.3566 5.87999 12.409 5.82398C13.1504 4.99088 14.128 4.40335 15.2116 4.13958C16.2952 3.87581 17.4335 3.94833 18.4749 4.34746C19.5163 4.7466 20.4114 5.45343 21.0411 6.37388C21.6708 7.29433 22.0053 8.38474 22 9.49998C22 11.79 20.5 13.5 19 15L13.508 20.313C13.3217 20.527 13.0919 20.6989 12.834 20.8173C12.5762 20.9357 12.296 20.9978 12.0123 20.9996C11.7285 21.0014 11.4476 20.9428 11.1883 20.8277C10.9289 20.7126 10.697 20.5436 10.508 20.332L5 15C3.5 13.5 2 11.8 2 9.49998Z"
                fill="white"
              />
            </mask>
            <g mask="url(#yirek)">
              <rect y="3" width="23" height="8" fill="#088408" />
              <rect y="14" width="23" height="8" fill="#FF0808" />
              <rect y="11" width="23" height="3" fill="white" />
            </g>
          </svg>
          <div>белән эшләнде</div>
        </div>
        <a
          target="_blank"
          className="text-muted-foreground mt-3 text-xs"
          href="https://github.com/hikariatama/room237"
          onClick={(e) => {
            e.preventDefault();
            void openUrl("https://github.com/hikariatama/room237");
          }}
        >
          hikariatama/room237
        </a>
        <div className="text-muted-foreground mt-1 text-xs">
          &copy; 2025 Daniil Gazizullin. GNU AGPL v4.0.
        </div>

        <div className="bg-background/70 border-border sticky bottom-0 mt-4 flex justify-between gap-2 rounded-full border p-2 backdrop-blur-lg">
          <Button variant="outline" onClick={onResetAll} disabled={loading}>
            {t("advanced.resetAll")}
          </Button>
          <div className="flex gap-2">
            <Button variant="ghost" onClick={() => setOpen(false)}>
              {t("advanced.cancel")}
            </Button>
            <Button onClick={onSave} disabled={loading}>
              {loading && <IconLoader2 className="h-4 w-4 animate-spin" />}
              {t("advanced.save")}
            </Button>
          </div>
        </div>
      </PopoverContent>
    </Popover>
  );
}
