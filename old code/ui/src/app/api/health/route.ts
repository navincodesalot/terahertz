import { NextResponse } from "next/server";

import { getRedis } from "@/lib/redis";

export const runtime = "nodejs";

export async function GET() {
  try {
    await getRedis().ping();

    return NextResponse.json({
      status: "ok",
      redis: "connected",
    });
  } catch (error) {
    console.error("Health check failed", error);

    return NextResponse.json(
      {
        status: "error",
        redis: "unavailable",
      },
      { status: 503 },
    );
  }
}
