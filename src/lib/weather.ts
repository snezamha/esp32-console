import type { ProjectConfig, ProjectLocation } from "@/lib/project-config";

/** Compact ASCII payload for the board; weather failure must not block device sync. */
export async function weatherPayload(settings: ProjectConfig, timeoutMs = 4000): Promise<string> {
  const location = settings.location as ProjectLocation;
  const unit = settings.unit === "fahrenheit" ? "fahrenheit" : "celsius";
  const query = new URLSearchParams({
    latitude: String(location.latitude),
    longitude: String(location.longitude),
    current: "temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m",
    daily: "temperature_2m_max,temperature_2m_min",
    forecast_days: "1",
    temperature_unit: unit,
  });
  try {
    const response = await fetch(`https://api.open-meteo.com/v1/forecast?${query}`, {
      next: { revalidate: 600 }, signal: AbortSignal.timeout(timeoutMs),
    });
    if (!response.ok) return "unavailable";
    const data = await response.json();
    const temperature = data.current?.temperature_2m;
    const apparent = data.current?.apparent_temperature;
    const humidity = data.current?.relative_humidity_2m;
    const wind = data.current?.wind_speed_10m;
    const high = data.daily?.temperature_2m_max?.[0];
    const low = data.daily?.temperature_2m_min?.[0];
    const code = data.current?.weather_code;
    if (typeof temperature !== "number" || !Number.isFinite(temperature) || typeof code !== "number") return "unavailable";
    const label = code === 0 ? "Clear" : code <= 3 ? "Cloudy" : code <= 48 ? "Fog" : code <= 67 ? "Rain" : code <= 77 ? "Snow" : code <= 82 ? "Showers" : code <= 86 ? "Snow showers" : "Thunderstorm";
    const degree = unit === "fahrenheit" ? "F" : "C";
    const details = settings.details === false ? label : [label,
      Number.isFinite(apparent) ? `Feels ${Math.round(apparent)} ${degree}` : "",
      Number.isFinite(humidity) ? `Humidity ${Math.round(humidity)}%` : "",
      Number.isFinite(wind) ? `Wind ${Math.round(wind)} km/h` : "",
      Number.isFinite(high) && Number.isFinite(low) ? `High ${Math.round(high)} Low ${Math.round(low)}` : "",
    ].filter(Boolean).join("~");
    return `${Math.round(temperature)} ${degree}|${details}`;
  } catch {
    return "unavailable";
  }
}
