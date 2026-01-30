import { Slot } from "@radix-ui/react-slot";
import { cva, type VariantProps } from "class-variance-authority";
import * as React from "react";

import { cn } from "@/lib/utils";
import { motion, type HTMLMotionProps } from "framer-motion";
import { Kbd } from "./kbd";

const buttonVariants = cva(
  "cursor-pointer inline-flex items-center justify-center gap-2 whitespace-nowrap rounded-3xl text-sm font-medium transition-colors disabled:pointer-events-none disabled:opacity-50 [&_svg]:pointer-events-none [&_svg:not([class*='size-'])]:size-4 shrink-0 [&_svg]:shrink-0 outline-none focus-visible:border-ring focus-visible:ring-ring/50 focus-visible:ring-[3px] aria-invalid:ring-destructive/20 dark:aria-invalid:ring-destructive/40 aria-invalid:border-destructive",
  {
    variants: {
      variant: {
        default:
          "bg-primary text-primary-foreground shadow-xs hover:bg-primary/90",
        destructive:
          "text-white shadow-xs bg-red-500/30 hover:bg-red-500/50 focus-visible:ring-red-500/20 dark:focus-visible:ring-red-500/40 dark:bg-red-500/60",
        outline:
          "border bg-background/30 shadow-xs dark:border-input hover:bg-primary/10",
        secondary:
          "bg-secondary text-secondary-foreground shadow-xs hover:bg-primary/10",
        ghost: "hover:bg-primary/10",
        link: "text-primary underline-offset-4 hover:underline",
      },
      size: {
        default: "px-4 py-2 has-[>svg]:px-3",
        sm: "text-xs gap-1.5 px-3 has-[>svg]:px-2.5 py-1.5",
        lg: "px-6 has-[>svg]:px-4",
        icon: "",
      },
    },
    defaultVariants: {
      variant: "default",
      size: "default",
    },
  },
);

function Button({
  className,
  variant,
  size,
  asChild = false,
  ...props
}: React.ComponentProps<"button"> &
  VariantProps<typeof buttonVariants> & {
    asChild?: boolean;
    keys?: string[];
    kbdSize?: "sm" | "md" | "lg";
  }) {
  if (asChild)
    return (
      <Slot
        data-slot="button"
        className={cn(buttonVariants({ variant, size, className }))}
        {...props}
      />
    );

  return (
    <motion.button
      data-slot="button"
      className={cn(buttonVariants({ variant, size, className }))}
      {...(props as Omit<HTMLMotionProps<"button">, "ref">)}
    >
      {props.keys ? (
        <span className="flex items-center">
          {props.children}
          <div className="ml-2">
            <Kbd
              keys={props.keys}
              size={props.kbdSize}
              variant={variant === "link" ? "outline" : variant}
            />
          </div>
        </span>
      ) : (
        props.children
      )}
    </motion.button>
  );
}

export { Button, buttonVariants };
