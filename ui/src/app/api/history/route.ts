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

    const present = records.filter(
      (record): record is MessageRecord => record !== null,
    );

    // Relayed image data is large. Only the newest record needs it (that's
    // what the receive panel renders); strip it from the rest.
    const trimmed = present.map((record, index) =>
      index === 0 || !record.telemetry?.receivedImageBase64
        ? record
        : {
            ...record,
            telemetry: {
              ...record.telemetry,
              receivedImageBase64: undefined,
            },
          },
    );

    return NextResponse.json({ records: trimmed });
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
