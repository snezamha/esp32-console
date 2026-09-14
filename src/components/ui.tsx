"use client";

import {
  Button,
  Dialog,
  DialogBackdrop,
  DialogPanel,
  DialogTitle,
  Field,
  Label,
  Switch,
} from "@headlessui/react";
import type { ReactNode } from "react";

export const primaryButton =
  "rounded-xl bg-zinc-900 text-sm font-medium text-white transition data-active:scale-[.99] data-disabled:cursor-not-allowed data-disabled:opacity-50 data-hover:bg-zinc-800 dark:bg-white dark:text-zinc-900 dark:data-hover:bg-zinc-200";
export const secondaryButton =
  "rounded-xl border border-zinc-200 text-sm font-medium transition data-disabled:cursor-not-allowed data-disabled:opacity-50 data-hover:bg-zinc-50 dark:border-zinc-700 dark:data-hover:bg-zinc-800";
export const accentButton =
  "rounded-xl bg-blue-500 text-sm font-medium text-white transition data-disabled:cursor-not-allowed data-disabled:opacity-50 data-hover:bg-blue-600";
export const inputClass =
  "h-11 w-full rounded-xl border border-zinc-200 bg-white px-3 text-sm text-zinc-900 outline-none focus:border-blue-500 focus:ring-2 focus:ring-blue-500/20 dark:border-zinc-800 dark:bg-zinc-900 dark:text-zinc-100";
export const cardClass = "rounded-2xl border border-zinc-200 bg-white dark:border-zinc-800 dark:bg-zinc-900/60";

/** Bottom sheet on phones, centered dialog on larger screens. */
export function Sheet({
  open,
  onClose,
  title,
  subtitle,
  children,
  footer,
}: {
  open: boolean;
  onClose: () => void;
  title: ReactNode;
  subtitle?: ReactNode;
  children: ReactNode;
  footer?: ReactNode;
}) {
  return (
    <Dialog open={open} onClose={onClose} className="relative z-50">
      <DialogBackdrop transition className="fixed inset-0 bg-black/40 transition duration-200 data-closed:opacity-0" />
      <div className="fixed inset-0 flex items-end justify-center p-4 sm:items-center">
        <DialogPanel
          transition
          className="flex max-h-[90dvh] w-full max-w-md flex-col gap-5 rounded-2xl bg-white p-5 shadow-xl transition duration-200 ease-out data-closed:translate-y-4 data-closed:opacity-0 dark:bg-zinc-900"
        >
          <div className="flex items-start justify-between gap-4">
            <div className="min-w-0 space-y-1">
              <DialogTitle className="truncate text-base font-semibold">{title}</DialogTitle>
              {subtitle && <div className="text-sm text-zinc-500">{subtitle}</div>}
            </div>
            <CloseButton onClick={onClose} />
          </div>
          <div className="-mx-5 min-h-0 flex-1 space-y-5 overflow-y-auto px-5">{children}</div>
          {footer && <div className="flex justify-end gap-2">{footer}</div>}
        </DialogPanel>
      </div>
    </Dialog>
  );
}

export function CloseButton({ onClick }: { onClick: () => void }) {
  return (
    <Button
      onClick={onClick}
      aria-label="Close"
      className="-m-1 rounded-lg p-1 text-zinc-500 data-hover:bg-zinc-100 dark:data-hover:bg-zinc-800"
    >
      <svg viewBox="0 0 20 20" fill="currentColor" className="size-5" aria-hidden>
        <path d="M6.28 5.22a.75.75 0 0 0-1.06 1.06L8.94 10l-3.72 3.72a.75.75 0 1 0 1.06 1.06L10 11.06l3.72 3.72a.75.75 0 1 0 1.06-1.06L11.06 10l3.72-3.72a.75.75 0 0 0-1.06-1.06L10 8.94 6.28 5.22Z" />
      </svg>
    </Button>
  );
}

export function Toggle({
  label,
  description,
  checked,
  onChange,
}: {
  label: string;
  description?: string;
  checked: boolean;
  onChange: (value: boolean) => void;
}) {
  return (
    <Field className="flex items-center justify-between gap-4">
      <div>
        <Label className="text-sm">{label}</Label>
        {description && <p className="text-xs text-zinc-500">{description}</p>}
      </div>
      <Switch
        checked={checked}
        onChange={onChange}
        className="group relative inline-flex h-7 w-12 shrink-0 cursor-pointer rounded-full bg-zinc-200 p-0.5 transition data-checked:bg-zinc-900 dark:bg-zinc-700 dark:data-checked:bg-white"
      >
        <span className="size-6 translate-x-0 rounded-full bg-white shadow transition group-data-checked:translate-x-5 dark:group-data-checked:bg-zinc-900" />
      </Switch>
    </Field>
  );
}

export function Slider({
  label,
  value,
  min,
  max,
  step,
  unit = "",
  onChange,
}: {
  label: string;
  value: number;
  min: number;
  max: number;
  step: number;
  unit?: string;
  onChange: (value: number) => void;
}) {
  return (
    <label className="block space-y-1.5">
      <span className="flex justify-between text-xs font-medium text-zinc-500">
        <span>{label}</span>
        <span className="tabular-nums">
          {value}
          {unit}
        </span>
      </span>
      <input
        type="range"
        min={min}
        max={max}
        step={step}
        value={value}
        onChange={(e) => onChange(Number(e.target.value))}
        className="w-full accent-zinc-900 dark:accent-white"
      />
    </label>
  );
}

export function Group({ title, children }: { title: string; children: ReactNode }) {
  return (
    <fieldset className="space-y-3">
      <legend className="mb-3 text-xs font-semibold tracking-wide text-zinc-400 uppercase">{title}</legend>
      {children}
    </fieldset>
  );
}

export function ErrorText({ children }: { children: ReactNode }) {
  return children ? <p className="text-sm text-red-600 dark:text-red-400">{children}</p> : null;
}
