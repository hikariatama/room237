/* eslint-disable @next/next/no-img-element */
"use client";

import {
  Popover,
  PopoverContent,
  PopoverTrigger,
} from "@/components/ui/popover";
import { useUpload } from "@/lib/hooks/use-upload";
import { useViewer } from "@/lib/hooks/use-viewer";
import { useRoom237 } from "@/lib/stores";
import { cn, copyFiles, extractItemFromState, isVideo } from "@/lib/utils";
import {
  IconClipboard,
  IconHeart,
  IconLoader2,
  IconTrash,
  IconX,
} from "@tabler/icons-react";
import { AnimatePresence, motion } from "framer-motion";
import { isEqual } from "lodash";
import {
  useCallback,
  useEffect,
  useMemo,
  useRef,
  useState,
  type PointerEvent as ReactPointerEvent,
  type WheelEvent as ReactWheelEvent,
} from "react";
import { createPortal } from "react-dom";
import { useStoreWithEqualityFn } from "zustand/traditional";
import { toast } from "./toaster";

const MIN_SCALE = 1;
const MAX_SCALE = 8;
const WHEEL_STEP = 0.03;
const PINCH_SENSITIVITY = 0.2;
const WHEEL_SEQUENCE_GAP_MS = 100;
const WHEEL_INERTIA_MS = 500;
const MINIMAP_WIDTH = 75;

export default function MediaViewer() {
  const viewer = useViewer();
  const { deleteMedia, toggleFavorite } = useUpload();
  const [mounted, setMounted] = useState(false);
  const bodyRef = useRef<HTMLElement | null>(null);

  const [isClosing, setIsClosing] = useState(false);
  const [copying, setCopying] = useState(false);
  const [deleteOpen, setDeleteOpen] = useState(false);
  const [cursor, setCursor] = useState<{ x: number; y: number }>({
    x: 0,
    y: 0,
  });

  useEffect(() => {
    bodyRef.current = document.body;
    setMounted(true);
  }, []);

  useEffect(() => {
    const updateCursor = (e: MouseEvent) => {
      setCursor({ x: e.clientX, y: e.clientY });
    };
    window.addEventListener("mousemove", updateCursor);
    return () => window.removeEventListener("mousemove", updateCursor);
  }, []);

  const close = useCallback(() => {
    setIsClosing(false);
    viewer.close();
  }, [viewer]);

  useEffect(() => {
    const h = (e: KeyboardEvent) => e.key === "Escape" && setIsClosing(true);
    window.addEventListener("keydown", h);
    return () => window.removeEventListener("keydown", h);
  }, []);

  const [scale, setScale] = useState(1);
  const [translation, setTranslation] = useState({ x: 0, y: 0 });
  const [naturalSize, setNaturalSize] = useState({ width: 0, height: 0 });
  const [containerSize, setContainerSize] = useState({ width: 0, height: 0 });
  const [baseSize, setBaseSize] = useState({ width: 0, height: 0 });

  const containerRef = useRef<HTMLDivElement>(null);
  const mediaRef = useRef<
    HTMLVideoElement | HTMLImageElement | null | undefined
  >(null);

  const scaleRef = useRef(scale);
  const translationRef = useRef(translation);

  useEffect(() => {
    scaleRef.current = scale;
  }, [scale]);

  useEffect(() => {
    translationRef.current = translation;
  }, [translation]);

  const item = useStoreWithEqualityFn(
    useRoom237,
    (state) => {
      if (viewer.viewerIndex === null) return null;
      return extractItemFromState({ state, index: viewer.viewerIndex });
    },
    isEqual,
  );

  const controlsRef = useRef<HTMLDivElement>(null);

  const controlsOpacity = useMemo(() => {
    if (!item) return 1;
    const containerRect = controlsRef.current?.getBoundingClientRect();
    if (!containerRect) return 1;
    const nearestX = Math.max(
      containerRect.left,
      Math.min(cursor.x, containerRect.right),
    );
    const nearestY = Math.max(
      containerRect.top,
      Math.min(cursor.y, containerRect.bottom),
    );

    const distance = Math.hypot(cursor.x - nearestX, cursor.y - nearestY);

    const maxDist = 150;
    return Math.max(
      0.15,
      Math.min(1, 1 - Math.min(distance, maxDist) / maxDist),
    );
  }, [cursor.x, cursor.y, item]);

  const controlsBackgroundOpacity = useMemo(
    () =>
      (0.1 +
        ((Math.min(1, Math.max(0.15, controlsOpacity)) - 0.15) / (1 - 0.15)) *
          (0.6 - 0.1)) *
      100,
    [controlsOpacity],
  );
  const controlsBackground = useMemo(
    () =>
      `color-mix(in oklab, var(--background) ${controlsBackgroundOpacity}%, transparent)`,
    [controlsBackgroundOpacity],
  );
  const controlsBackdropBlur = useMemo(() => {
    const t = (controlsBackgroundOpacity - 10) / (60 - 10);
    const blur = Math.max(0, Math.min(12, t * 12));
    return `blur(${blur}px)`;
  }, [controlsBackgroundOpacity]);

  const rect = useMemo(() => {
    if (
      !item ||
      typeof window === "undefined" ||
      typeof document === "undefined" ||
      !mounted
    )
      return undefined;
    const el = document.querySelector<HTMLImageElement>(
      `[data-img-url="${item.name}"]`,
    );
    if (!el) return undefined;
    const { x, y, width, height } = el.getBoundingClientRect();
    return {
      x: x - window.innerWidth / 2 + width / 2,
      y: y - window.innerHeight / 2 + width / 2,
      width,
      height,
      scale: 0.7,
    };
  }, [item, mounted]);

  const init = !isClosing
    ? (rect ?? { scale: 0.9, opacity: 0 })
    : { x: 0, y: 0, width: "auto", height: "auto", scale: 1, opacity: 1 };
  const anim = !isClosing
    ? { x: 0, y: 0, width: "auto", height: "auto", scale: 1, opacity: 1 }
    : (rect ?? { scale: 0.9, opacity: 0 });

  const updateMeasurements = useCallback(() => {
    const containerEl = containerRef.current;
    if (containerEl) {
      const { width, height } = containerEl.getBoundingClientRect();
      setContainerSize({ width, height });
    }
    const mediaEl = mediaRef.current;
    if (mediaEl) {
      const { width, height } = mediaEl.getBoundingClientRect();
      const currentScale = scaleRef.current || 1;
      if (width && height) {
        setBaseSize({
          width: width / currentScale,
          height: height / currentScale,
        });
        return;
      }
    }
    if (containerEl && naturalSize.width && naturalSize.height) {
      const containerRatio = containerEl.clientWidth / containerEl.clientHeight;
      const mediaRatio = naturalSize.width / naturalSize.height;
      if (mediaRatio > containerRatio) {
        const width = containerEl.clientWidth;
        setBaseSize({ width, height: width / mediaRatio });
      } else {
        const height = containerEl.clientHeight;
        setBaseSize({ width: height * mediaRatio, height });
      }
    }
  }, [naturalSize.height, naturalSize.width]);

  useEffect(() => {
    updateMeasurements();
    const observer =
      typeof ResizeObserver !== "undefined"
        ? new ResizeObserver(() => updateMeasurements())
        : null;
    if (observer && containerRef.current)
      observer.observe(containerRef.current);
    window.addEventListener("resize", updateMeasurements);
    window.addEventListener("orientationchange", updateMeasurements);
    return () => {
      if (observer) observer.disconnect();
      window.removeEventListener("resize", updateMeasurements);
      window.removeEventListener("orientationchange", updateMeasurements);
    };
  }, [updateMeasurements]);

  useEffect(() => {
    if (!mediaRef.current || typeof ResizeObserver === "undefined") return;
    const obs = new ResizeObserver(() => updateMeasurements());
    obs.observe(mediaRef.current);
    return () => obs.disconnect();
  }, [item?.name, updateMeasurements]);

  useEffect(() => {
    setScale(1);
    setTranslation({ x: 0, y: 0 });
    setNaturalSize({ width: 0, height: 0 });
    setBaseSize({ width: 0, height: 0 });
    scaleRef.current = 1;
    translationRef.current = { x: 0, y: 0 };
  }, [item?.name]);

  const clampScale = useCallback(
    (next: number) => Math.min(MAX_SCALE, Math.max(MIN_SCALE, next)),
    [],
  );

  const derivedBaseSize = useMemo(() => baseSize, [baseSize]);

  const clampTranslation = useCallback(
    (x: number, y: number, nextScale: number) => {
      if (!containerSize.width || !derivedBaseSize.width) {
        return { x: 0, y: 0 };
      }
      const scaledWidth = derivedBaseSize.width * nextScale;
      const scaledHeight = derivedBaseSize.height * nextScale;
      const maxIconX = Math.max(0, (scaledWidth - containerSize.width) / 2);
      const maxY = Math.max(0, (scaledHeight - containerSize.height) / 2);
      return {
        x: Math.max(-maxIconX, Math.min(maxIconX, x)),
        y: Math.max(-maxY, Math.min(maxY, y)),
      };
    },
    [
      containerSize.height,
      containerSize.width,
      derivedBaseSize.height,
      derivedBaseSize.width,
    ],
  );

  const commitTransform = useCallback(
    (nextScale: number, nextTranslation: { x: number; y: number }) => {
      const clampedScale = clampScale(nextScale);
      const clampedTranslation = clampTranslation(
        nextTranslation.x,
        nextTranslation.y,
        clampedScale,
      );
      setScale(clampedScale);
      setTranslation(clampedTranslation);
      scaleRef.current = clampedScale;
      translationRef.current = clampedTranslation;
    },
    [clampScale, clampTranslation],
  );

  useEffect(() => {
    commitTransform(scaleRef.current, translationRef.current);
  }, [
    commitTransform,
    derivedBaseSize.height,
    derivedBaseSize.width,
    containerSize.height,
    containerSize.width,
  ]);

  const getRelativePoint = useCallback((clientX: number, clientY: number) => {
    const rect = containerRef.current?.getBoundingClientRect();
    if (!rect) return { x: 0, y: 0 };
    return {
      x: clientX - (rect.left + rect.width / 2),
      y: clientY - (rect.top + rect.height / 2),
    };
  }, []);

  const zoomTo = useCallback(
    (targetScale: number, focal?: { x: number; y: number }) => {
      if (!derivedBaseSize.width || !derivedBaseSize.height) return;
      const origin = focal ?? { x: 0, y: 0 };
      const prevScale = scaleRef.current;
      const nextScale = clampScale(targetScale);
      const factor = prevScale ? nextScale / prevScale : 1;
      const prevTranslation = translationRef.current;
      const nextTranslation = {
        x: (prevTranslation.x - origin.x) * factor + origin.x,
        y: (prevTranslation.y - origin.y) * factor + origin.y,
      };
      commitTransform(nextScale, nextTranslation);
    },
    [
      clampScale,
      commitTransform,
      derivedBaseSize.height,
      derivedBaseSize.width,
    ],
  );

  const panBy = useCallback(
    (dx: number, dy: number) => {
      if (
        scaleRef.current <= 1 ||
        !derivedBaseSize.width ||
        !derivedBaseSize.height
      )
        return;
      const prev = translationRef.current;
      commitTransform(scaleRef.current, { x: prev.x + dx, y: prev.y + dy });
    },
    [commitTransform, derivedBaseSize.height, derivedBaseSize.width],
  );

  const resetTransform = useCallback(() => {
    commitTransform(1, { x: 0, y: 0 });
  }, [commitTransform]);

  const pointersRef = useRef<Map<number, { x: number; y: number }>>(new Map());
  const pinchStartRef = useRef<{
    distance: number;
    scale: number;
    center: { x: number; y: number };
  } | null>(null);
  const lastPanRef = useRef<{ x: number; y: number } | null>(null);
  const lastNonCtrlWheelRef = useRef<number>(0);
  const lastWheelTsRef = useRef<number>(0);
  const wheelSequenceCtrlAllowedRef = useRef<boolean>(false);

  const handlePointerDown = useCallback(
    (e: ReactPointerEvent<HTMLDivElement>) => {
      if (!containerRef.current) return;
      if (e.button === 2) return;
      containerRef.current.setPointerCapture(e.pointerId);
      pointersRef.current.set(e.pointerId, { x: e.clientX, y: e.clientY });
      if (pointersRef.current.size === 2) {
        const points = Array.from(pointersRef.current.values());
        const distance = Math.hypot(
          points[0]!.x - points[1]!.x,
          points[0]!.y - points[1]!.y,
        );
        const center = {
          x: (points[0]!.x + points[1]!.x) / 2,
          y: (points[0]!.y + points[1]!.y) / 2,
        };
        pinchStartRef.current = {
          distance,
          scale: scaleRef.current,
          center,
        };
      } else {
        lastPanRef.current = { x: e.clientX, y: e.clientY };
      }
    },
    [],
  );

  const handlePointerMove = useCallback(
    (e: ReactPointerEvent<HTMLDivElement>) => {
      if (!pointersRef.current.has(e.pointerId)) return;
      pointersRef.current.set(e.pointerId, { x: e.clientX, y: e.clientY });
      if (pointersRef.current.size === 2 && pinchStartRef.current) {
        const points = Array.from(pointersRef.current.values());
        const distance = Math.hypot(
          points[0]!.x - points[1]!.x,
          points[0]!.y - points[1]!.y,
        );
        const center = {
          x: (points[0]!.x + points[1]!.x) / 2,
          y: (points[0]!.y + points[1]!.y) / 2,
        };
        const start = pinchStartRef.current;
        const focal = getRelativePoint(center.x, center.y);
        const rawScale = distance / start.distance;
        const scaleFactor = 1 + (rawScale - 1) * PINCH_SENSITIVITY;
        zoomTo(start.scale * scaleFactor, focal);
      } else if (scaleRef.current > 1 && lastPanRef.current) {
        const dx = e.clientX - lastPanRef.current.x;
        const dy = e.clientY - lastPanRef.current.y;
        panBy(dx, dy);
        lastPanRef.current = { x: e.clientX, y: e.clientY };
      }
    },
    [getRelativePoint, panBy, zoomTo],
  );

  const releasePointer = useCallback((id: number) => {
    pointersRef.current.delete(id);
    if (pointersRef.current.size < 2) {
      pinchStartRef.current = null;
    }
    if (pointersRef.current.size === 1) {
      const remaining = Array.from(pointersRef.current.values())[0];
      lastPanRef.current = remaining ?? null;
    } else {
      lastPanRef.current = null;
    }
  }, []);

  const handlePointerUp = useCallback(
    (e: ReactPointerEvent<HTMLDivElement>) => {
      releasePointer(e.pointerId);
      if (containerRef.current?.hasPointerCapture(e.pointerId)) {
        containerRef.current.releasePointerCapture(e.pointerId);
      }
    },
    [releasePointer],
  );

  const handleWheel = useCallback(
    (e: ReactWheelEvent<HTMLDivElement>) => {
      if (!item) return;
      const now = performance.now();
      const ctrlActive = e.ctrlKey || e.metaKey;
      if (now - lastWheelTsRef.current > WHEEL_SEQUENCE_GAP_MS) {
        wheelSequenceCtrlAllowedRef.current = ctrlActive;
      }
      lastWheelTsRef.current = now;
      if (ctrlActive) {
        const inertiaActive =
          now - lastNonCtrlWheelRef.current < WHEEL_INERTIA_MS;
        if (!wheelSequenceCtrlAllowedRef.current || inertiaActive) {
          return;
        }
        e.preventDefault();
        e.stopPropagation();
        const focal = getRelativePoint(e.clientX, e.clientY);
        const direction = e.deltaY > 0 ? -1 : 1;
        const factor = direction > 0 ? 1 + WHEEL_STEP : 1 / (1 + WHEEL_STEP);
        zoomTo(scaleRef.current * factor, focal);
      } else if (scaleRef.current > 1) {
        e.preventDefault();
        e.stopPropagation();
        panBy(-e.deltaX, -e.deltaY);
        lastNonCtrlWheelRef.current = performance.now();
      } else {
        lastNonCtrlWheelRef.current = performance.now();
        wheelSequenceCtrlAllowedRef.current = false;
      }
    },
    [getRelativePoint, item, panBy, zoomTo],
  );

  useEffect(() => {
    if (viewer.viewerIndex === null) return;
    const handleKey = (e: KeyboardEvent) => {
      if (!(e.metaKey || e.ctrlKey)) return;
      if (["=", "+", "-", "0"].includes(e.key)) {
        e.preventDefault();
        const focal = { x: 0, y: 0 };
        if (e.key === "0") {
          resetTransform();
          return;
        }
        const factor = e.key === "-" ? 1 / (1 + WHEEL_STEP) : 1 + WHEEL_STEP;
        zoomTo(scaleRef.current * factor, focal);
      }
    };
    window.addEventListener("keydown", handleKey);
    return () => window.removeEventListener("keydown", handleKey);
  }, [resetTransform, viewer.viewerIndex, zoomTo]);

  const content = (
    <AnimatePresence>
      {item && mounted && (
        <motion.div
          key="overlay"
          className={cn(
            "fixed inset-0 z-50 flex items-center justify-center rounded-3xl bg-black/70 backdrop-blur-sm",
            isClosing && "pointer-events-none",
          )}
          initial={{ opacity: 0 }}
          animate={{ opacity: isClosing ? 0 : 1 }}
          exit={{ opacity: 0 }}
          onClick={() => setIsClosing(true)}
        >
          <motion.div
            initial={init}
            animate={anim}
            onAnimationComplete={() => isClosing && close()}
            exit={{ scale: 1.2, opacity: 0 }}
            transition={{ type: "spring", stiffness: 1200, damping: 50 }}
            className="relative flex max-h-[90vh] max-w-[90vw]"
            onClick={(e) => e.stopPropagation()}
          >
            {item && (
              <div
                className="absolute top-4 right-4 z-50 flex gap-2 text-white"
                ref={controlsRef}
              >
                <motion.button
                  whileHover={{ scale: 1.1 }}
                  whileTap={{ scale: 0.9 }}
                  transition={{ type: "spring", stiffness: 500, damping: 25 }}
                  className="flex size-7 cursor-pointer items-center justify-center rounded-full border transition-[color] hover:text-red-500"
                  style={{
                    backgroundColor: controlsBackground,
                    backdropFilter: controlsBackdropBlur,
                  }}
                  onClick={() => toggleFavorite(item)}
                >
                  <IconHeart
                    className={cn("size-4", item.favorite && "text-red-500")}
                    style={{ opacity: controlsOpacity }}
                    fill={item.favorite ? "currentColor" : "none"}
                  />
                </motion.button>
                <motion.button
                  whileHover={{ scale: 1.1 }}
                  whileTap={{ scale: 0.9 }}
                  transition={{ type: "spring", stiffness: 500, damping: 25 }}
                  className="flex size-7 cursor-pointer items-center justify-center rounded-full border"
                  style={{
                    backgroundColor: controlsBackground,
                    backdropFilter: controlsBackdropBlur,
                  }}
                  onClick={async () => {
                    setCopying(true);
                    try {
                      await copyFiles([item]);
                    } catch (err) {
                      console.error(err);
                      toast.error("Failed to copy");
                    } finally {
                      setCopying(false);
                    }
                  }}
                  disabled={copying}
                >
                  {copying ? (
                    <IconLoader2
                      className="size-4 animate-spin"
                      style={{ opacity: controlsOpacity }}
                    />
                  ) : (
                    <IconClipboard
                      className="size-4"
                      style={{ opacity: controlsOpacity }}
                    />
                  )}
                </motion.button>
                <Popover open={deleteOpen} onOpenChange={setDeleteOpen}>
                  <PopoverTrigger asChild>
                    <motion.button
                      whileHover={{ scale: 1.1 }}
                      whileTap={{ scale: 0.9 }}
                      transition={{
                        type: "spring",
                        stiffness: 500,
                        damping: 25,
                      }}
                      className="flex size-7 cursor-pointer items-center justify-center rounded-full border transition-[color] hover:text-red-500"
                      style={{
                        backgroundColor: controlsBackground,
                        backdropFilter: controlsBackdropBlur,
                      }}
                      onClick={() => setDeleteOpen(true)}
                    >
                      <IconTrash
                        className="size-4"
                        style={{ opacity: controlsOpacity }}
                      />
                    </motion.button>
                  </PopoverTrigger>
                  <PopoverContent className="w-fit space-y-3" align="end">
                    <p className="text-secondary-foreground w-full text-center text-sm">
                      You sure?
                    </p>
                    <div className="flex gap-2">
                      <motion.button
                        whileHover={{ scale: 1.1 }}
                        whileTap={{ scale: 0.9 }}
                        transition={{
                          type: "spring",
                          stiffness: 500,
                          damping: 25,
                        }}
                        className="text-destructive-foreground cursor-pointer rounded-full bg-red-500/90 px-3 py-2 text-sm font-semibold transition-colors hover:bg-red-500"
                        onClick={async () => {
                          await deleteMedia(item);
                          setDeleteOpen(false);
                          close();
                        }}
                      >
                        <IconTrash className="size-4" />
                      </motion.button>
                      <motion.button
                        whileHover={{ scale: 1.1 }}
                        whileTap={{ scale: 0.9 }}
                        transition={{
                          type: "spring",
                          stiffness: 500,
                          damping: 25,
                        }}
                        className="hover:bg-muted cursor-pointer rounded-full border px-3 py-2 text-sm font-semibold transition-colors"
                        onClick={() => setDeleteOpen(false)}
                      >
                        <IconX className="size-4" />
                      </motion.button>
                    </div>
                  </PopoverContent>
                </Popover>
              </div>
            )}
            <div
              ref={containerRef}
              className="relative flex max-h-[90vh] max-w-[90vw] touch-none items-center justify-center overflow-hidden rounded-3xl bg-black/30"
              onWheel={handleWheel}
              onPointerDown={handlePointerDown}
              onPointerMove={handlePointerMove}
              onPointerUp={handlePointerUp}
              onPointerCancel={handlePointerUp}
              onPointerLeave={handlePointerUp}
            >
              {scale > 1 && derivedBaseSize.width > 0 && (
                <div className="pointer-events-none absolute top-4 left-4 z-40 text-white">
                  <div
                    className="relative overflow-hidden rounded-lg border border-white/20 bg-black/40 shadow-lg backdrop-blur-md"
                    style={{
                      width: MINIMAP_WIDTH,
                      height:
                        naturalSize.width && naturalSize.height
                          ? MINIMAP_WIDTH *
                            (naturalSize.height / naturalSize.width)
                          : MINIMAP_WIDTH,
                    }}
                  >
                    {isVideo(item.name) ? (
                      <video
                        src={item.url}
                        muted
                        playsInline
                        className="h-full w-full object-contain"
                      />
                    ) : (
                      <img
                        src={item.url}
                        alt="minimap"
                        className="h-full w-full object-contain"
                      />
                    )}
                    {(() => {
                      const scaledWidth = derivedBaseSize.width * scale;
                      const scaledHeight = derivedBaseSize.height * scale;
                      const miniWidth = MINIMAP_WIDTH;
                      const miniHeight =
                        naturalSize.width && naturalSize.height
                          ? MINIMAP_WIDTH *
                            (naturalSize.height / naturalSize.width)
                          : MINIMAP_WIDTH;

                      if (!scaledWidth || !scaledHeight) return null;

                      if (
                        scaledWidth <= containerSize.width &&
                        scaledHeight <= containerSize.height
                      ) {
                        return (
                          <div className="absolute inset-0">
                            <div
                              className="absolute border border-white/80 bg-white/20 backdrop-blur-[1px]"
                              style={{
                                width: miniWidth,
                                height: miniHeight,
                                transform: "translate(0px, 0px)",
                                borderRadius: `${Math.min(miniWidth, miniHeight) * 0.0533333333}px`,
                              }}
                            />
                          </div>
                        );
                      }

                      const imgLeft = -scaledWidth / 2 + translation.x;
                      const imgTop = -scaledHeight / 2 + translation.y;
                      const viewLeft = -containerSize.width / 2;
                      const viewTop = -containerSize.height / 2;
                      const viewRight = viewLeft + containerSize.width;
                      const viewBottom = viewTop + containerSize.height;
                      const imgRight = imgLeft + scaledWidth;
                      const imgBottom = imgTop + scaledHeight;

                      const interLeft = Math.max(imgLeft, viewLeft);
                      const interTop = Math.max(imgTop, viewTop);
                      const interRight = Math.min(imgRight, viewRight);
                      const interBottom = Math.min(imgBottom, viewBottom);

                      const visibleWidth = Math.max(0, interRight - interLeft);
                      const visibleHeight = Math.max(0, interBottom - interTop);

                      const rectWidth =
                        (visibleWidth / scaledWidth) * miniWidth;
                      const rectHeight =
                        (visibleHeight / scaledHeight) * miniHeight;

                      const rectX =
                        ((interLeft - imgLeft) / scaledWidth) * miniWidth;
                      const rectY =
                        ((interTop - imgTop) / scaledHeight) * miniHeight;

                      return (
                        <div className="absolute inset-0">
                          <div
                            className="absolute rounded-lg border border-white/80 bg-white/20 backdrop-blur-[1px]"
                            style={{
                              width: Math.min(miniWidth, rectWidth),
                              height: Math.min(miniHeight, rectHeight),
                              transform: `translate(${Math.max(
                                0,
                                Math.min(miniWidth - rectWidth - 2, rectX),
                              )}px, ${Math.max(
                                0,
                                Math.min(miniHeight - rectHeight - 2, rectY),
                              )}px)`,
                            }}
                          />
                        </div>
                      );
                    })()}
                  </div>
                </div>
              )}
              {isVideo(item.name) ? (
                <video
                  ref={(el) => {
                    mediaRef.current = el;
                  }}
                  src={item.url}
                  controls={scale <= 1.0001}
                  autoPlay
                  playsInline
                  onLoadedMetadata={(e) => {
                    setNaturalSize({
                      width: e.currentTarget.videoWidth,
                      height: e.currentTarget.videoHeight,
                    });
                    updateMeasurements();
                  }}
                  className="max-h-[90vh] max-w-[90vw] object-contain select-none"
                  style={{
                    transform: `translate3d(${translation.x}px, ${translation.y}px, 0) scale(${scale})`,
                    transformOrigin: "center center",
                    willChange: "transform",
                  }}
                />
              ) : (
                <img
                  ref={(el) => {
                    mediaRef.current = el;
                  }}
                  src={item.url}
                  className="max-h-[90vh] max-w-[90vw] object-contain select-none"
                  alt="media"
                  draggable={false}
                  onLoad={(e) => {
                    setNaturalSize({
                      width: e.currentTarget.naturalWidth,
                      height: e.currentTarget.naturalHeight,
                    });
                    updateMeasurements();
                  }}
                  style={{
                    transform: `translate3d(${translation.x}px, ${translation.y}px, 0) scale(${scale})`,
                    transformOrigin: "center center",
                    willChange: "transform",
                  }}
                />
              )}
            </div>
          </motion.div>
        </motion.div>
      )}
    </AnimatePresence>
  );

  if (!mounted || !bodyRef.current) {
    return null;
  }

  return createPortal(content, bodyRef.current);
}
