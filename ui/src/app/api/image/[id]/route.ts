import { NextResponse } from "next/server";

import { getRedis } from "@/lib/redis";
import { imageKey, type StoredImage } from "@/lib/protocol";

export const runtime = "nodejs";

// Returns the original uploaded image for a message so the dashboard can show
// it side by side with what the receiver actually reassembled.
export async function GET(
  _request: Request,
  { params }: { params: Promise<{ id: string }> },
) {
  const { id } = await params;

  try {
    const redis = getRedis();
    const stored = await redis.get<StoredImage>(imageKey(id));
    if (!stored) {
      return NextResponse.json({ error: "Image not found" }, { status: 404 });
    }
    return NextResponse.json({ image: stored });
  } catch (error) {
    console.error("Original image read failed", error);
    return NextResponse.json(
      { error: "Image is unavailable" },
      { status: 503 },
    );
  }
}
