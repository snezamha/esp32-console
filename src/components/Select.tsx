"use client";

import {
  Field,
  Label,
  Listbox,
  ListboxButton,
  ListboxOption,
  ListboxOptions,
} from "@headlessui/react";
import type { ReactNode } from "react";

type SelectProps<T> = {
  label: string;
  value: T | null;
  options: T[];
  onChange: (value: T) => void;
  getKey: (option: T) => string;
  renderValue: (option: T) => ReactNode;
  renderOption?: (option: T) => ReactNode;
  placeholder?: string;
  disabled?: boolean;
};

export function Select<T>({
  label,
  value,
  options,
  onChange,
  getKey,
  renderValue,
  renderOption = renderValue,
  placeholder = "Select…",
  disabled,
}: SelectProps<T>) {
  return (
    <Field disabled={disabled} className="space-y-1.5">
      <Label className="text-xs font-medium text-zinc-500 data-disabled:opacity-50">
        {label}
      </Label>
      <Listbox
        value={value}
        onChange={(v) => v !== null && onChange(v)}
        by={(a, b) => a != null && b != null && getKey(a as T) === getKey(b as T)}
      >
        <ListboxButton className="flex h-12 w-full items-center justify-between gap-3 rounded-xl border border-zinc-200 bg-white px-4 text-left text-sm outline-none transition focus-visible:ring-2 focus-visible:ring-zinc-900/20 data-disabled:opacity-50 dark:border-zinc-800 dark:bg-zinc-900 dark:focus-visible:ring-white/20">
          <span className="min-w-0 flex-1 truncate">
            {value ? (
              renderValue(value)
            ) : (
              <span className="text-zinc-400">{placeholder}</span>
            )}
          </span>
          <ChevronIcon />
        </ListboxButton>
        <ListboxOptions
          anchor="bottom"
          transition
          className="z-20 max-h-72 w-(--button-width) rounded-xl border border-zinc-200 bg-white p-1 text-sm shadow-lg outline-none [--anchor-gap:6px] transition duration-100 ease-out data-closed:scale-95 data-closed:opacity-0 dark:border-zinc-800 dark:bg-zinc-900"
        >
          {options.map((option) => (
            <ListboxOption
              key={getKey(option)}
              value={option}
              className="group flex cursor-pointer items-center gap-2 rounded-lg px-3 py-2.5 select-none data-focus:bg-zinc-100 dark:data-focus:bg-zinc-800"
            >
              <CheckIcon />
              <div className="min-w-0 flex-1">{renderOption(option)}</div>
            </ListboxOption>
          ))}
        </ListboxOptions>
      </Listbox>
    </Field>
  );
}

function ChevronIcon() {
  return (
    <svg viewBox="0 0 20 20" className="size-4 shrink-0 text-zinc-400" fill="currentColor" aria-hidden>
      <path d="M5.23 7.21a.75.75 0 0 1 1.06.02L10 11.17l3.71-3.94a.75.75 0 1 1 1.08 1.04l-4.25 4.5a.75.75 0 0 1-1.08 0l-4.25-4.5a.75.75 0 0 1 .02-1.06Z" />
    </svg>
  );
}

function CheckIcon() {
  return (
    <svg viewBox="0 0 20 20" className="invisible size-4 shrink-0 group-data-selected:visible" fill="currentColor" aria-hidden>
      <path d="M16.7 5.3a1 1 0 0 1 0 1.4l-8 8a1 1 0 0 1-1.4 0l-4-4a1 1 0 1 1 1.4-1.4L8 12.58l7.3-7.3a1 1 0 0 1 1.4 0Z" />
    </svg>
  );
}
