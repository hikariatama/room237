"use client";

import * as React from "react";
import * as ContextMenuPrimitive from "@radix-ui/react-context-menu";
import { AnimatePresence, motion } from "framer-motion";

import { cn } from "@/lib/utils";

const ContextMenuOpenContext = React.createContext(false);
const useContextMenuOpen = () => React.useContext(ContextMenuOpenContext);

type ContextMenuProps = React.ComponentPropsWithoutRef<
  typeof ContextMenuPrimitive.Root
> & {
  open?: boolean;
};

const ContextMenu = (props: ContextMenuProps) => {
  const [internalOpen, setInternalOpen] = React.useState(false);
  const isControlled = props.open !== undefined;
  const open = isControlled ? props.open! : internalOpen;

  const handleChange = React.useCallback(
    (next: boolean) => {
      if (!isControlled) setInternalOpen(next);
      props.onOpenChange?.(next);
    },
    [isControlled, props],
  );

  return (
    <ContextMenuOpenContext.Provider value={open}>
      <ContextMenuPrimitive.Root {...props} onOpenChange={handleChange} />
    </ContextMenuOpenContext.Provider>
  );
};

const ContextMenuTrigger = ContextMenuPrimitive.Trigger;

const ContextMenuGroup = ContextMenuPrimitive.Group;

const ContextMenuPortal = ContextMenuPrimitive.Portal;

const ContextMenuSub = ContextMenuPrimitive.Sub;

const ContextMenuRadioGroup = ContextMenuPrimitive.RadioGroup;

const transition = {
  type: "spring",
  stiffness: 500,
  damping: 30,
} as const;

type ContextMenuContentProps = React.ComponentPropsWithoutRef<
  typeof ContextMenuPrimitive.Content
> & {
  sideOffset?: number;
};

const ContextMenuContent = React.forwardRef<
  React.ComponentRef<typeof ContextMenuPrimitive.Content>,
  ContextMenuContentProps
>(({ className, sideOffset = 8, children, style, ...props }, ref) => {
  const open = useContextMenuOpen();
  return (
    <ContextMenuPortal forceMount>
      <AnimatePresence>
        {open && (
          <ContextMenuPrimitive.Content ref={ref} {...props} forceMount asChild>
            <motion.div
              key="context-menu"
              initial={{ opacity: 0, scale: 0.6, y: -6 }}
              animate={{ opacity: 1, scale: 1, y: 0 }}
              exit={{ opacity: 0, scale: 0.6, y: -6 }}
              transition={transition}
              style={
                {
                  ...(style ?? {}),
                  "--radix-context-menu-content-side-offset": `${sideOffset}px`,
                } as React.CSSProperties
              }
              className={cn(
                "bg-background/80 text-foreground relative z-200 min-w-40 origin-(--radix-context-menu-content-transform-origin) rounded-2xl border border-white/10 p-1 shadow-xl backdrop-blur-xl outline-none",
                className,
              )}
            >
              {children}
            </motion.div>
          </ContextMenuPrimitive.Content>
        )}
      </AnimatePresence>
    </ContextMenuPortal>
  );
});
ContextMenuContent.displayName = ContextMenuPrimitive.Content.displayName;

const ContextMenuItem = React.forwardRef<
  React.ComponentRef<typeof ContextMenuPrimitive.Item>,
  React.ComponentPropsWithoutRef<typeof ContextMenuPrimitive.Item> & {
    inset?: boolean;
  }
>(({ className, inset, ...props }, ref) => (
  <ContextMenuPrimitive.Item
    ref={ref}
    className={cn(
      "focus:bg-primary/10 focus:text-foreground relative flex cursor-pointer items-center gap-2 rounded-xl px-3 py-2 text-sm transition-colors outline-none select-none data-disabled:pointer-events-none data-disabled:opacity-50",
      inset && "pl-8",
      className,
    )}
    {...props}
  />
));
ContextMenuItem.displayName = ContextMenuPrimitive.Item.displayName;

const ContextMenuCheckboxItem = React.forwardRef<
  React.ComponentRef<typeof ContextMenuPrimitive.CheckboxItem>,
  React.ComponentPropsWithoutRef<typeof ContextMenuPrimitive.CheckboxItem>
>(({ className, children, checked, ...props }, ref) => (
  <ContextMenuPrimitive.CheckboxItem
    ref={ref}
    className={cn(
      "focus:bg-primary/10 focus:text-foreground relative flex cursor-pointer items-center gap-2 rounded-xl py-2 pr-3 pl-8 text-sm transition-colors outline-none select-none data-disabled:pointer-events-none data-disabled:opacity-50",
      className,
    )}
    checked={checked}
    {...props}
  >
    <span className="absolute left-2 flex h-3.5 w-3.5 items-center justify-center">
      <ContextMenuPrimitive.ItemIndicator>
        <div className="bg-primary h-2.5 w-2.5 rounded-sm" />
      </ContextMenuPrimitive.ItemIndicator>
    </span>
    {children}
  </ContextMenuPrimitive.CheckboxItem>
));
ContextMenuCheckboxItem.displayName =
  ContextMenuPrimitive.CheckboxItem.displayName;

const ContextMenuRadioItem = React.forwardRef<
  React.ComponentRef<typeof ContextMenuPrimitive.RadioItem>,
  React.ComponentPropsWithoutRef<typeof ContextMenuPrimitive.RadioItem>
>(({ className, children, ...props }, ref) => (
  <ContextMenuPrimitive.RadioItem
    ref={ref}
    className={cn(
      "focus:bg-primary/10 focus:text-foreground relative flex cursor-pointer items-center gap-2 rounded-xl py-2 pr-3 pl-8 text-sm transition-colors outline-none select-none data-disabled:pointer-events-none data-disabled:opacity-50",
      className,
    )}
    {...props}
  >
    <span className="absolute left-2 flex h-3.5 w-3.5 items-center justify-center">
      <ContextMenuPrimitive.ItemIndicator>
        <div className="bg-primary/90 h-2 w-2 rounded-full" />
      </ContextMenuPrimitive.ItemIndicator>
    </span>
    {children}
  </ContextMenuPrimitive.RadioItem>
));
ContextMenuRadioItem.displayName = ContextMenuPrimitive.RadioItem.displayName;

const ContextMenuLabel = React.forwardRef<
  React.ComponentRef<typeof ContextMenuPrimitive.Label>,
  React.ComponentPropsWithoutRef<typeof ContextMenuPrimitive.Label> & {
    inset?: boolean;
  }
>(({ className, inset, ...props }, ref) => (
  <ContextMenuPrimitive.Label
    ref={ref}
    className={cn(
      "text-muted-foreground px-2 py-1.5 text-xs font-medium",
      inset && "pl-8",
      className,
    )}
    {...props}
  />
));
ContextMenuLabel.displayName = ContextMenuPrimitive.Label.displayName;

const ContextMenuSeparator = React.forwardRef<
  React.ComponentRef<typeof ContextMenuPrimitive.Separator>,
  React.ComponentPropsWithoutRef<typeof ContextMenuPrimitive.Separator>
>(({ className, ...props }, ref) => (
  <ContextMenuPrimitive.Separator
    ref={ref}
    className={cn("bg-border/40 my-1 h-px", className)}
    {...props}
  />
));
ContextMenuSeparator.displayName = ContextMenuPrimitive.Separator.displayName;

const ContextMenuShortcut = ({
  className,
  ...props
}: React.HTMLAttributes<HTMLSpanElement>) => {
  return (
    <span
      className={cn("text-muted-foreground ml-auto text-xs", className)}
      {...props}
    />
  );
};
ContextMenuShortcut.displayName = "ContextMenuShortcut";

const ContextMenuSubTrigger = React.forwardRef<
  React.ComponentRef<typeof ContextMenuPrimitive.SubTrigger>,
  React.ComponentPropsWithoutRef<typeof ContextMenuPrimitive.SubTrigger> & {
    inset?: boolean;
  }
>(({ className, inset, children, ...props }, ref) => (
  <ContextMenuPrimitive.SubTrigger
    ref={ref}
    className={cn(
      "focus:bg-primary/10 focus:text-foreground relative flex cursor-pointer items-center gap-2 rounded-xl px-3 py-2 text-sm transition-colors outline-none select-none data-[state=open]:bg-white/10",
      inset && "pl-8",
      className,
    )}
    {...props}
  >
    {children}
  </ContextMenuPrimitive.SubTrigger>
));
ContextMenuSubTrigger.displayName = ContextMenuPrimitive.SubTrigger.displayName;

const ContextMenuSubContent = React.forwardRef<
  React.ComponentRef<typeof ContextMenuPrimitive.SubContent>,
  React.ComponentPropsWithoutRef<typeof ContextMenuPrimitive.SubContent>
>(({ className, ...props }, ref) => (
  <ContextMenuPrimitive.Portal>
    <ContextMenuPrimitive.SubContent
      ref={ref}
      className={cn(
        "bg-background/85 text-foreground data-[state=open]:animate-in data-[state=closed]:animate-out data-[state=closed]:fade-out-0 data-[state=open]:fade-in-0 data-[side=bottom]:slide-in-from-top-1 data-[side=left]:slide-in-from-right-1 data-[side=right]:slide-in-from-left-1 data-[side=top]:slide-in-from-bottom-1 min-w-40 overflow-hidden rounded-2xl border border-white/10 p-1 shadow-xl backdrop-blur-xl",
        className,
      )}
      sideOffset={8}
      {...props}
      // @ts-expect-error prevent weird inheritance
      forceMount={props.forceMount ?? false}
    />
  </ContextMenuPrimitive.Portal>
));
ContextMenuSubContent.displayName = ContextMenuPrimitive.SubContent.displayName;

export {
  ContextMenu,
  ContextMenuTrigger,
  ContextMenuContent,
  ContextMenuItem,
  ContextMenuCheckboxItem,
  ContextMenuRadioItem,
  ContextMenuLabel,
  ContextMenuSeparator,
  ContextMenuShortcut,
  ContextMenuGroup,
  ContextMenuPortal,
  ContextMenuSub,
  ContextMenuSubContent,
  ContextMenuSubTrigger,
  ContextMenuRadioGroup,
};
