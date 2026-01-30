"use client";

import { Button } from "@/components/ui/button";
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from "@/components/ui/select";
import { cn } from "@/lib/utils";
import { AnimatePresence, motion } from "framer-motion";
import { IconChevronLeft, IconChevronRight } from "@tabler/icons-react";
import { useCallback, useMemo } from "react";
import { Input } from "../ui/input";

const MONTHS = [
  "Jan",
  "Feb",
  "Mar",
  "Apr",
  "May",
  "Jun",
  "Jul",
  "Aug",
  "Sep",
  "Oct",
  "Nov",
  "Dec",
];

const WEEKDAYS = ["Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"];

export function DateTimePicker({
  value,
  onChange,
}: {
  value: Date;
  onChange: (next: Date) => void;
}) {
  const monthStart = useMemo(
    () => new Date(value.getFullYear(), value.getMonth(), 1),
    [value],
  );
  const daysInMonth = useMemo(
    () => new Date(value.getFullYear(), value.getMonth() + 1, 0).getDate(),
    [value],
  );
  const leadingEmpty = monthStart.getDay();
  const paddedDays = useMemo(
    () => [
      ...Array.from({ length: leadingEmpty }, () => null),
      ...Array.from({ length: daysInMonth }, (_, idx) => idx + 1),
    ],
    [daysInMonth, leadingEmpty],
  );
  const years = useMemo(() => {
    const currentYear = new Date().getFullYear();
    const minYear = Math.min(currentYear, value.getFullYear()) - 60;
    const maxYear = Math.max(currentYear, value.getFullYear()) + 10;
    return Array.from(
      { length: maxYear - minYear + 1 },
      (_, idx) => minYear + idx,
    );
  }, [value]);

  const clampDay = useCallback((next: Date) => {
    const maxDay = new Date(
      next.getFullYear(),
      next.getMonth() + 1,
      0,
    ).getDate();
    if (next.getDate() > maxDay) {
      next.setDate(maxDay);
    }
    return next;
  }, []);

  const handleMonthChange = useCallback(
    (delta: number) => {
      const next = clampDay(
        new Date(
          value.getFullYear(),
          value.getMonth() + delta,
          value.getDate(),
          value.getHours(),
          value.getMinutes(),
        ),
      );
      onChange(next);
    },
    [clampDay, onChange, value],
  );

  const handleMonthSelect = useCallback(
    (monthIndex: number) => {
      const next = clampDay(
        new Date(
          value.getFullYear(),
          monthIndex,
          value.getDate(),
          value.getHours(),
          value.getMinutes(),
        ),
      );
      onChange(next);
    },
    [clampDay, onChange, value],
  );

  const handleYearSelect = useCallback(
    (year: number) => {
      const next = clampDay(
        new Date(
          year,
          value.getMonth(),
          value.getDate(),
          value.getHours(),
          value.getMinutes(),
        ),
      );
      onChange(next);
    },
    [clampDay, onChange, value],
  );

  const handleDaySelect = useCallback(
    (day: number) => {
      const next = new Date(value);
      next.setDate(day);
      onChange(next);
    },
    [onChange, value],
  );

  const handleTimeSelect = useCallback(
    (type: "hour" | "minute", nextValue: number) => {
      const next = new Date(value);
      if (type === "hour") {
        next.setHours(nextValue);
      } else {
        next.setMinutes(nextValue);
      }
      next.setSeconds(0, 0);
      onChange(next);
    },
    [onChange, value],
  );

  return (
    <motion.div
      initial={{ opacity: 0, y: -6 }}
      animate={{ opacity: 1, y: 0 }}
      exit={{ opacity: 0, y: 6 }}
      transition={{ duration: 0.15 }}
      className="space-y-3"
    >
      <div className="flex items-center justify-between gap-2">
        <div className="flex items-center gap-1">
          <Button
            variant="ghost"
            size="icon"
            className="h-8 w-8"
            onClick={() => handleMonthChange(-1)}
          >
            <IconChevronLeft className="size-4" />
          </Button>
          <Button
            variant="ghost"
            size="icon"
            className="h-8 w-8"
            onClick={() => handleMonthChange(1)}
          >
            <IconChevronRight className="size-4" />
          </Button>
        </div>
        <div className="flex items-center gap-2">
          <Select
            value={String(value.getMonth())}
            onValueChange={(month) => handleMonthSelect(Number(month))}
          >
            <SelectTrigger className="min-w-27.5">
              <SelectValue>{MONTHS[value.getMonth()]}</SelectValue>
            </SelectTrigger>
            <SelectContent className="z-1000 max-h-64">
              {MONTHS.map((month, idx) => (
                <SelectItem key={month} value={String(idx)}>
                  {month}
                </SelectItem>
              ))}
            </SelectContent>
          </Select>
          <Select
            value={String(value.getFullYear())}
            onValueChange={(year) => handleYearSelect(Number(year))}
          >
            <SelectTrigger className="min-w-22.5">
              <SelectValue>{value.getFullYear()}</SelectValue>
            </SelectTrigger>
            <SelectContent className="z-1000 max-h-64">
              {years.map((year) => (
                <SelectItem key={year} value={String(year)}>
                  {year}
                </SelectItem>
              ))}
            </SelectContent>
          </Select>
        </div>
      </div>

      <div className="text-muted-foreground grid grid-cols-7 gap-1 text-center text-[11px]">
        {WEEKDAYS.map((day) => (
          <span key={day} className="rounded-md py-1 font-medium">
            {day}
          </span>
        ))}
      </div>

      <div className="grid grid-cols-7 gap-1">
        <AnimatePresence>
          {paddedDays.map((day, idx) =>
            day ? (
              <motion.button
                key={`day-${day}`}
                layoutId={`day-${day}`}
                layout
                type="button"
                className={cn(
                  "size-8 cursor-pointer rounded-xl text-sm font-medium transition-colors",
                  day === value.getDate() ? "bg-white/20" : "hover:bg-white/10",
                )}
                initial={{ opacity: 0 }}
                animate={{ opacity: 1 }}
                exit={{ opacity: 0 }}
                whileHover={{ scale: 1.02 }}
                whileTap={{ scale: 0.97 }}
                transition={{
                  type: "spring",
                  duration: 0.3,
                  delay: day * 0.004,
                  scale: { delay: 0 },
                }}
                onClick={() => handleDaySelect(day)}
              >
                {day}
              </motion.button>
            ) : (
              <span key={`pad-${idx}`} className="size-8" />
            ),
          )}
        </AnimatePresence>
      </div>

      <div className="flex w-full items-center justify-center gap-1.5">
        <Input
          type="number"
          value={String(value.getHours()).padStart(2, "0")}
          onChange={(e) => handleTimeSelect("hour", Number(e.target.value))}
          className="w-14 [appearance:textfield] text-center [&::-webkit-inner-spin-button]:appearance-none [&::-webkit-outer-spin-button]:appearance-none"
        />
        <span className="text-muted-foreground text-sm">:</span>
        <Input
          type="number"
          value={String(value.getMinutes()).padStart(2, "0")}
          onChange={(e) => handleTimeSelect("minute", Number(e.target.value))}
          className="w-14 [appearance:textfield] text-center [&::-webkit-inner-spin-button]:appearance-none [&::-webkit-outer-spin-button]:appearance-none"
        />
      </div>
    </motion.div>
  );
}
