"use client";

import { toast } from "@/components/toaster";
import { Button } from "@/components/ui/button";
import {
  ContextMenuItem,
  ContextMenuSub,
  ContextMenuSubContent,
  ContextMenuSubTrigger,
} from "@/components/ui/context-menu";
import { DateTimePicker } from "@/components/ui/date-time-picker";
import { useUpload } from "@/lib/hooks/use-upload";
import { useI18n } from "@/lib/i18n";
import { useRoom237 } from "@/lib/stores";
import { extractItemFromState } from "@/lib/utils";
import { IconCalendarClock } from "@tabler/icons-react";
import { isEqual } from "lodash";
import { useCallback, useEffect, useMemo, useState } from "react";
import { useStoreWithEqualityFn } from "zustand/traditional";

export const ChangeDateContextMenuItem = ({
  mediaPath,
}: {
  mediaPath: string;
}) => {
  const { t } = useI18n();
  const { updateMediaDates } = useUpload();

  const [open, setOpen] = useState(false);

  const selectionCount = useRoom237((state) => state.selection.length);
  const metaDates = useStoreWithEqualityFn(
    useRoom237,
    (state) => {
      const item = extractItemFromState({ state, path: mediaPath });
      return {
        shoot: item?.meta.shoot,
        added: item?.meta.added,
      };
    },
    isEqual,
  );

  const defaultDate = useMemo(() => {
    const base =
      metaDates.shoot ?? metaDates.added ?? Math.floor(Date.now() / 1000);
    return new Date(base * 1000);
  }, [metaDates.shoot, metaDates.added]);

  const [selectedDate, setSelectedDate] = useState<Date>(defaultDate);
  useEffect(() => setSelectedDate(defaultDate), [defaultDate]);

  const handleSubmit = useCallback(async () => {
    const state = useRoom237.getState();
    const selection = state.selection;
    const item = extractItemFromState({ state, path: mediaPath });
    const todo = selection.length > 0 ? selection : item ? [item] : [];
    const ts = selectedDate.getTime();
    if (Number.isNaN(ts)) {
      toast.error("Invalid date");
      return;
    }
    if (ts < 0) {
      toast.error("Date must be on or after 1970-01-01");
      return;
    }
    try {
      await updateMediaDates(todo, Math.floor(ts / 1000));
      toast.success("Date updated");
    } catch (error) {
      console.error(error);
      const message =
        error instanceof Error ? error.message : "Failed to update date";
      toast.error(message);
    }
  }, [selectedDate, updateMediaDates, mediaPath]);

  return (
    <ContextMenuSub
      open={open}
      onOpenChange={(next) => !next && setOpen(false)}
    >
      <ContextMenuSubTrigger
        className="gap-2"
        onPointerMove={(e) => e.preventDefault()}
        onPointerLeave={(e) => e.preventDefault()}
        onSelect={(e) => e.preventDefault()}
        onClick={(e) => {
          e.preventDefault();
          setOpen((prev) => !prev);
        }}
        asChild
      >
        <Button variant="ghost" className="w-full justify-start">
          <IconCalendarClock className="size-4" />
          {selectionCount > 1
            ? t("contextMenu.changeDateFor", { count: selectionCount })
            : t("contextMenu.changeDate")}
        </Button>
      </ContextMenuSubTrigger>
      <ContextMenuSubContent>
        <div key="date-menu" className="space-y-3 p-1">
          <DateTimePicker value={selectedDate} onChange={setSelectedDate} />
          <div className="flex justify-end gap-2 px-1 pb-1">
            <ContextMenuItem asChild>
              <Button
                type="button"
                variant="outline"
                size="sm"
                onClick={() => setOpen(false)}
              >
                {t("contextMenu.cancel")}
              </Button>
            </ContextMenuItem>
            <ContextMenuItem asChild>
              <Button
                type="button"
                size="sm"
                onClick={() => void handleSubmit()}
              >
                {t("contextMenu.save")}
              </Button>
            </ContextMenuItem>
          </div>
        </div>
      </ContextMenuSubContent>
    </ContextMenuSub>
  );
};
