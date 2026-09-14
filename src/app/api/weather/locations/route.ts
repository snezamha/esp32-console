import { getUser, unauthorized } from "@/lib/auth";
export async function GET(request: Request) {
  if (!(await getUser())) return unauthorized();
  const name = new URL(request.url).searchParams.get("q")?.trim().slice(0, 100) ?? "";
  if (name.length < 2) return Response.json({ locations: [] });
  const query = new URLSearchParams({ name, count: "30", language: /[\u0600-\u06ff]/.test(name) ? "fa" : "en", format: "json" });
  try {
    const response = await fetch(`https://geocoding-api.open-meteo.com/v1/search?${query}`, { next: { revalidate: 86400 }, signal: AbortSignal.any([request.signal, AbortSignal.timeout(5000)]) });
    if (!response.ok) throw new Error("Location search unavailable. Try again.");
    const data = await response.json();
    const locations = (Array.isArray(data.results) ? data.results : []).filter((r: Record<string, unknown>) => typeof r.name === "string" && typeof r.latitude === "number" && typeof r.longitude === "number" && Math.abs(r.latitude) <= 90 && Math.abs(r.longitude) <= 180).map((r: Record<string, unknown>) => ({ id: r.id, name: r.name, country: r.country ?? "", region: r.admin1 ?? "", latitude: r.latitude, longitude: r.longitude, timezone: r.timezone ?? "" }));
    return Response.json({ locations });
  } catch { return Response.json({ error: "City search unavailable. Retry or enter coordinates manually." }, { status: 502 }); }
}
