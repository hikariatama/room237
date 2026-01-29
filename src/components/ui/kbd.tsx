import {
  IconArrowBarRight,
  IconArrowBigUp,
  IconArrowDown,
  IconArrowLeft,
  IconArrowRight,
  IconArrowUp,
  IconBackspace,
  IconCommand,
  IconCornerDownLeft,
  IconHttpDelete,
  IconCommand as IconOption,
} from "@tabler/icons-react";
import type { VariantProps } from "class-variance-authority";
import { cva } from "class-variance-authority";
import { useMemo } from "react";
import { PiMouseLeftClickFill, PiMouseRightClickFill } from "react-icons/pi";

import { cn, useOS } from "@/lib/utils";

const kbdVariants = cva(
  "h-5 min-w-5 gap-1 px-1 text-xs font-medium inline-flex w-fit items-center justify-center rounded-sm font-sans select-none",
  {
    variants: {
      variant: {
        default: "bg-background/10 text-muted-foreground",
        destructive: "bg-red-600 text-red-100",
        outline: "bg-input/50 text-muted-foreground",
        secondary: "border border-input/80 bg-secondary text-muted-foreground",
        ghost: "border border-input/80 bg-transparent text-muted-foreground",
      },
      size: {
        sm: "h-4 min-w-4 text-[10px]",
        md: "h-5 min-w-5 text-xs",
        lg: "h-6 min-w-6 text-sm",
      },
    },
    defaultVariants: {
      variant: "default",
      size: "md",
    },
  },
);

function KbdPrimitive({
  kbdKey,
  variant,
  size,
  className,
  ...props
}: React.ComponentProps<"kbd"> & { kbdKey: string } & VariantProps<
    typeof kbdVariants
  >) {
  const os = useOS();

  const icons = useMemo(() => {
    return {
      enter: IconCornerDownLeft,
      meta: os === "macos" ? IconCommand : "Ctrl",
      option: os === "macos" ? IconOption : "Alt",
      backspace: IconBackspace,
      shift: IconArrowBigUp,
      tab: IconArrowBarRight,
      arrowleft: IconArrowLeft,
      arrowright: IconArrowRight,
      arrowdown: IconArrowDown,
      arrowup: IconArrowUp,
      click: PiMouseLeftClickFill,
      rightclick: PiMouseRightClickFill,
      delete: IconHttpDelete,
    };
  }, [os]);

  return (
    <kbd
      data-slot="kbd"
      className={cn(
        kbdVariants({ variant, size, className }),
        (!!icons[kbdKey.toLowerCase() as keyof typeof icons] ||
          kbdKey.length === 1) &&
          "aspect-square",
      )}
      {...props}
    >
      {icons[kbdKey.toLowerCase() as keyof typeof icons] ? (
        (() => {
          const IconComponent =
            icons[kbdKey.toLowerCase() as keyof typeof icons];
          if (typeof IconComponent === "string") {
            return <span className="select-none">{IconComponent}</span>;
          }
          return <IconComponent className="size-3" />;
        })()
      ) : (
        <span className="select-none">{kbdKey}</span>
      )}
    </kbd>
  );
}

function Kbd({
  keys,
  className,
  variant,
  size,
  ...props
}: React.ComponentProps<"div"> & { keys: string[] } & VariantProps<
    typeof kbdVariants
  >) {
  const os = useOS();

  const keysMapped = useMemo(() => {
    if (
      os === "macos" &&
      keys.length === 1 &&
      keys[0]?.toLowerCase() === "delete"
    ) {
      return ["meta", "backspace"];
    }

    return keys;
  }, [os, keys]);

  return (
    <kbd
      data-slot="kbd-group"
      className={cn("inline-flex items-center gap-1", className)}
      {...props}
    >
      {keysMapped.map((key, index) => (
        <KbdPrimitive kbdKey={key} key={index} variant={variant} size={size} />
      ))}
    </kbd>
  );
}

export { Kbd, KbdPrimitive };
