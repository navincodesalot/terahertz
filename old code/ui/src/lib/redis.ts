import { Redis } from "@upstash/redis";

let redis: Redis | undefined;

export function getRedis() {
  if (!redis) {
    const url = process.env.UPSTASH_REDIS_REST_URL;
    const token = process.env.UPSTASH_REDIS_REST_TOKEN;

    if (!url || !token) {
      throw new Error(
        "Missing UPSTASH_REDIS_REST_URL or UPSTASH_REDIS_REST_TOKEN",
      );
    }

    redis = new Redis({ url, token });
  }

  return redis;
}
