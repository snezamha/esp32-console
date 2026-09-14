import type { DeviceSettings } from "@/lib/device-settings";

/** Compact ASCII payload for the board; weather failure must not block device sync. */
export async function weatherPayload(settings: DeviceSettings, timeoutMs = 4000): Promise<string> {
  const query = new URLSearchParams({
    latitude: String(settings.weather_lat / 10000),
    longitude: String(settings.weather_lon / 10000),
    current: "temperature_2m,weather_code",
    temperature_unit: settings.weather_unit,
  });
  try {
    const response = await fetch(`https://api.open-meteo.com/v1/forecast?${query}`, {
      next: { revalidate: 600 }, signal: AbortSignal.timeout(timeoutMs),
    });
    if (!response.ok) return "unavailable";
    const data = await response.json();
    const temperature = data.current?.temperature_2m;
    const code = data.current?.weather_code;
    if (typeof temperature !== "number" || !Number.isFinite(temperature) || typeof code !== "number") return "unavailable";
    const label = code === 0 ? "Clear" : code <= 3 ? "Cloudy" : code <= 48 ? "Fog" : code <= 67 ? "Rain" : code <= 77 ? "Snow" : code <= 82 ? "Showers" : code <= 86 ? "Snow showers" : "Thunderstorm";
    return `${Math.round(temperature)} ${settings.weather_unit === "fahrenheit" ? "F" : "C"}|${label}`;
  } catch {
    return "unavailable";
  }
}
