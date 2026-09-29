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
import { useCallback, useEffect, useState, type ReactNode } from "react";

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
  wide,
}: {
  open: boolean;
  onClose: () => void;
  title: ReactNode;
  subtitle?: ReactNode;
  children: ReactNode;
  footer?: ReactNode;
  /** Room for lists and editors instead of a compact form. */
  wide?: boolean;
}) {
  return (
    <Dialog open={open} onClose={onClose} className="relative z-50">
      <DialogBackdrop transition className="fixed inset-0 bg-black/40 transition duration-200 data-closed:opacity-0" />
      <div className="fixed inset-0 flex items-end justify-center p-4 sm:items-center">
        <DialogPanel
          transition
          className={`flex max-h-[90dvh] w-full ${wide ? "max-w-2xl" : "max-w-md"} flex-col gap-5 rounded-2xl bg-white p-5 shadow-xl transition duration-200 ease-out data-closed:translate-y-4 data-closed:opacity-0 dark:bg-zinc-900`}
        >
          <div className="flex items-start justify-between gap-4">
            <div className="min-w-0 space-y-1">
              <DialogTitle className="truncate text-base font-semibold">{title}</DialogTitle>
              {subtitle && <div className="text-sm text-zinc-500">{subtitle}</div>}
            </div>
            <CloseButton onClick={onClose} />
          </div>
          <div className="-mx-5 min-h-0 flex-1 space-y-5 overflow-y-auto px-5">{children}</div>
          {footer && <div className="flex flex-wrap items-center justify-end gap-2">{footer}</div>}
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

/** Form-level failure. `role="alert"` so a screen reader announces it when it appears. */
export function ErrorText({ children }: { children: ReactNode }) {
  return children ? (
    <p role="alert" className="text-sm text-red-600 dark:text-red-400">
      {children}
    </p>
  ) : null;
}

/** One shown message. The id makes repeats distinct, so the same text shown twice restarts. */
export type Toast = { text: string; id: number };

/**
 * Transient bottom-of-screen message for things that succeeded. Use with
 * `<ToastBanner toast={toast} />`; a failure the user may need to read or retry belongs in an
 * `ErrorText` or a banner that stays on screen instead.
 */
export function useToast() {
  const [toast, setToast] = useState<Toast | null>(null);
  const show = useCallback((text: string) => setToast({ text, id: nextToastId++ }), []);
  useEffect(() => {
    if (!toast) return;
    const timer = setTimeout(() => setToast(null), 3000);
    return () => clearTimeout(timer);
  }, [toast]);
  return [toast, show] as const;
}

// Showing the same text twice would otherwise leave the state unchanged, so the effect would not
// re-run, the first timer would still be counting, and the second toast would vanish early.
let nextToastId = 0;

export function ToastBanner({ toast }: { toast: Toast | null }) {
  if (!toast) return null;
  return (
    <div
      key={toast.id}
      role="status"
      className="fixed inset-x-4 bottom-[max(1rem,env(safe-area-inset-bottom))] z-40 mx-auto max-w-sm rounded-xl bg-zinc-900 px-4 py-3 text-center text-sm text-white shadow-lg dark:bg-white dark:text-zinc-900"
    >
      {toast.text}
    </div>
  );
}

/** Failure that stays on screen until the user reads it, with an optional retry. */
export function ErrorBanner({ message, onRetry, onDismiss }: { message: string; onRetry?: () => void; onDismiss?: () => void }) {
  return (
    <div
      role="alert"
      className="flex flex-wrap items-center justify-between gap-2 rounded-xl bg-red-50 p-3 text-sm text-red-700 dark:bg-red-950/50 dark:text-red-300"
    >
      <span className="min-w-0 flex-1 break-words">{message}</span>
      <span className="flex shrink-0 gap-3">
        {onRetry && <Button onClick={onRetry} className="font-medium underline">Retry</Button>}
        {onDismiss && <Button onClick={onDismiss} aria-label="Dismiss error" className="font-medium underline">Dismiss</Button>}
      </span>
    </div>
  );
}

/** Reusable destructive/neutral confirmation sheet. */
export function ConfirmDialog({
  open,
  onClose,
  onConfirm,
  title,
  description,
  confirmLabel = "Confirm",
  danger = true,
  busy,
}: {
  open: boolean;
  onClose: () => void;
  onConfirm: () => void;
  title: ReactNode;
  description: ReactNode;
  confirmLabel?: string;
  danger?: boolean;
  busy?: boolean;
}) {
  return (
    <Sheet
      open={open}
      onClose={onClose}
      title={title}
      footer={
        <>
          <Button onClick={onClose} className={secondaryButton + " h-10 px-4"}>
            Cancel
          </Button>
          <Button
            onClick={onConfirm}
            disabled={busy}
            className={
              danger
                ? "h-10 rounded-xl bg-red-600 px-4 text-sm font-medium text-white data-hover:bg-red-500 data-disabled:cursor-not-allowed data-disabled:opacity-50"
                : accentButton + " h-10 px-4"
            }
          >
            {confirmLabel}
          </Button>
        </>
      }
    >
      <p className="text-sm text-zinc-500">{description}</p>
    </Sheet>
  );
}

/** Pulsing placeholder block for content still loading. */
export function Skeleton({ className = "" }: { className?: string }) {
  return <div className={`animate-pulse rounded-xl bg-zinc-200 dark:bg-zinc-800 ${className}`} aria-hidden />;
}
