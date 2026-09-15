"use client";
import { Button } from "@headlessui/react";
import { useEffect, useRef, useState } from "react";
import { ErrorText, inputClass, secondaryButton } from "@/components/ui";
import type { ProjectLocation } from "@/lib/project-config";
type Location = { id: number; name: string; country: string; region: string; latitude: number; longitude: number; timezone: string };
export function WeatherProjectSettings({ value, onChange }: { value: ProjectLocation; onChange: (location: ProjectLocation) => void }) {
  const lat = String(value.latitude), lon = String(value.longitude);
  const [query, setQuery] = useState("");
  const [locations, setLocations] = useState<Location[]>([]);
  const [searching, setSearching] = useState(false);
  const [locating, setLocating] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [label, setLabel] = useState("");
  const [searched, setSearched] = useState(false);
  const positionRevision = useRef(0);
  const mounted = useRef(true);
  useEffect(() => { mounted.current = true; return () => { mounted.current = false; }; }, []);
  useEffect(() => {
    const controller = new AbortController();
    if (query.trim().length < 2) return;
    const timer = setTimeout(async () => {
      setSearching(true); setError(null);
      try {
        const response = await fetch(`/api/weather/locations?q=${encodeURIComponent(query.trim())}`, { signal: controller.signal });
        const data = await response.json();
        if (!response.ok) throw new Error(data.error);
        if (!controller.signal.aborted) { setLocations(data.locations); setSearched(true); }
      } catch (err) { if (!controller.signal.aborted) setError(err instanceof Error ? err.message : "Search failed."); }
      finally { if (!controller.signal.aborted) setSearching(false); }
    }, 350);
    return () => { clearTimeout(timer); controller.abort(); };
  }, [query]);
  const coordinates = (a: string, b: string, name: string) => { positionRevision.current++; onChange({ latitude: Number(a), longitude: Number(b), label: name }); setLabel(name); };
  const locate = () => {
    setError(null);
    if (!window.isSecureContext || !navigator.geolocation) { setError("Location requires HTTPS and a browser with location support. You can search for your city instead."); return; }
    setLocating(true); const revision = ++positionRevision.current;
    navigator.geolocation.getCurrentPosition((position) => {
      if (!mounted.current || revision !== positionRevision.current) return;
      onChange({ latitude: Number(position.coords.latitude.toFixed(4)), longitude: Number(position.coords.longitude.toFixed(4)), label: "Current location" });
      setLabel(`Current location · accuracy ±${Math.round(position.coords.accuracy)} m`); setLocating(false);
    }, (failure) => {
      if (!mounted.current || revision !== positionRevision.current) return;
      setLocating(false); setError(failure.code === 1 ? "Location permission denied. Allow location in browser settings or search for a city." : failure.code === 3 ? "Location request timed out. Retry or search for a city." : "Current location unavailable. Search for a city or enter coordinates.");
    }, { enableHighAccuracy: true, timeout: 15000, maximumAge: 60000 });
  };
  return <div className="space-y-4">
    <label className="block space-y-1 text-xs">Search cities worldwide<input value={query} onChange={(e) => { setQuery(e.target.value); setLocations([]); setSearched(false); setSearching(false); }} placeholder="City, postal code or city, country…" className={inputClass} autoComplete="off" /></label>
    {searching && <p role="status" className="text-xs text-zinc-500">Searching cities…</p>}
    {!searching && searched && locations.length === 0 && <p role="status" className="text-xs text-zinc-500">No matches. Try the local or English name, or add the country.</p>}
    {locations.length > 0 && <ul aria-label="Matching cities" className="max-h-52 overflow-auto rounded-lg border border-zinc-200 dark:border-zinc-700">{locations.map((city) => <li key={city.id}><Button className="w-full px-3 py-2 text-left text-xs hover:bg-zinc-100 dark:hover:bg-zinc-800" onClick={() => { coordinates(city.latitude.toFixed(4), city.longitude.toFixed(4), [city.name, city.region, city.country].filter(Boolean).join(" · ")); setLocating(false); }}><span className="block font-medium">{city.name}</span><span className="text-zinc-500">{[city.region, city.country].filter(Boolean).join(" · ")} · {city.latitude.toFixed(4)}, {city.longitude.toFixed(4)}</span></Button></li>)}</ul>}
    <Button disabled={locating} onClick={locate} className={secondaryButton + " h-10 w-full"}>{locating ? "Requesting location…" : "Use my current location"}</Button>
    <p className="text-xs text-zinc-500">Location is requested only when you press this button. Your browser asks for permission; coordinates are saved when you save settings or load Weather.</p>
    <ErrorText>{error}</ErrorText>
    {label && <p role="status" className="rounded-lg bg-blue-50 p-3 text-xs text-blue-700 dark:bg-blue-950 dark:text-blue-300">{label}</p>}
    <details><summary className="cursor-pointer text-xs">Precise coordinates</summary><div className="mt-3 grid grid-cols-2 gap-2"><label className="text-xs">Latitude<input type="number" min="-90" max="90" step="0.0001" value={lat} onChange={(e) => { coordinates(e.target.value, lon, "Custom coordinates"); setLocating(false); }} className={inputClass} /></label><label className="text-xs">Longitude<input type="number" min="-180" max="180" step="0.0001" value={lon} onChange={(e) => { coordinates(lat, e.target.value, "Custom coordinates"); setLocating(false); }} className={inputClass} /></label></div></details>
    <p className="text-xs text-zinc-500">Selected coordinates: {lat}, {lon}. Weather refreshes from cached readings every 10 minutes. <a href="https://open-meteo.com/en/docs/geocoding-api" target="_blank" rel="noreferrer" className="underline">Location data: Open-Meteo / GeoNames</a>.</p>
  </div>;
}
