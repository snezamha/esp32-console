import { PrismaPg } from "@prisma/adapter-pg";
import { PrismaClient } from "@prisma/client";

// Reused across invocations on the same warm serverless instance, so a request does not open a
// new pool per call. Point DATABASE_URL at Neon's *pooled* connection string (the one with
// `-pooler` in the hostname) so Vercel's many concurrent instances don't exceed Postgres'
// connection limit.
const globals = globalThis as typeof globalThis & { __prisma?: PrismaClient };

function create() {
  const url = process.env.DATABASE_URL;
  if (!url) {
    throw new Error(
      "DATABASE_URL is not set. Create a Neon Postgres database, put its pooled connection " +
        "string in DATABASE_URL (Vercel project settings, or .env.local for local dev), then " +
        "run `pnpm db:push`.",
    );
  }
  return (globals.__prisma ??= new PrismaClient({ adapter: new PrismaPg({ connectionString: url }) }));
}

// Lazy: the connection is not opened (and DATABASE_URL not required) until a query actually
// runs, so `next build` works before the database is configured.
export const db = new Proxy({} as PrismaClient, {
  get: (_target, prop) => Reflect.get(create() as object, prop, create()),
});
