import { NextResponse } from "next/server";

import { getRedis } from "@/lib/redis";
import { HISTORY_KEY, messageKey, type MessageRecord } from "@/lib/protocol";

export const runtime = "nodejs";

export async function GET() {
  try {
    const redis = getRedis();
    const ids = await redis.zrange<string[]>(HISTORY_KEY, 0, 49, { rev: true });
    const records = await Promise.all(
      ids.map((id) => redis.get<MessageRecord>(messageKey(id))),
    );

    return NextResponse.json({
      records: records.filter(
        (record): record is MessageRecord => record !== null,
      ),
    });
  } catch (error) {
    console.error("History read failed", error);
    return NextResponse.json(
      { error: "History is unavailable" },
      { status: 503 },
    );
  }
}

export async function DELETE() {
  try {
    const redis = getRedis();
    const ids = await redis.zrange<string[]>(HISTORY_KEY, 0, -1);
    if (ids.length > 0) await redis.del(...ids.map(messageKey));
    await redis.del(HISTORY_KEY);
    return NextResponse.json({ cleared: true });
  } catch (error) {
    console.error("History clear failed", error);
    return NextResponse.json(
      { error: "History could not be cleared" },
      { status: 503 },
    );
  }
}
