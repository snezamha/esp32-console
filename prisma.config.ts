import { defineConfig } from "prisma/config";

// Used by `prisma db push` / `prisma studio` / `prisma generate` (not by the app at runtime —
// see src/lib/db.ts). The placeholder lets `generate` run before DATABASE_URL is configured;
// `db push` and `studio` need a real one in the environment (e.g. .env.local).
export default defineConfig({
  schema: "prisma/schema.prisma",
  datasource: {
    url: process.env.DATABASE_URL ?? "postgresql://placeholder/placeholder",
  },
});
