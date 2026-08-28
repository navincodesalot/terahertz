import { NextResponse } from "next/server";
import { z } from "zod";

import { getRedis } from "@/lib/redis";
import { messageKey, type MessageRecord } from "@/lib/protocol";

export const runtime = "nodejs";

const telemetrySchema = z.object({
  id: z.string().min(1),
  status: z.enum(["success", "failed"]),
  type: z.enum(["text", "image"]),
  payload: z.string().optional(),
  output: z
    .object({
      text: z.string().optional(),
      bytes: z.number().int().nonnegative().optional(),
    })
    .optional(),
  radio: z
    .object({
      baud: z.number().int().positive().optional(),
      uart: z.string().optional(),
    })
    .optional(),
  performance: z
    .object({
      transmissionMs: z.number().nonnegative().optional(),
      bitErrors: z.number().int().nonnegative().optional(),
      checksumPassed: z.boolean().optional(),
      retries: z.number().int().nonnegative().optional(),
    })
    .optional(),
  timestamp: z.number().int().nonnegative().optional(),
});

export async function POST(request: Request) {
  let body: unknown;
  try {
    body = await request.json();
  } catch {
    return NextResponse.json(
      { error: "Request body must be valid JSON" },
      { status: 400 },
    );
  }

  const parsed = telemetrySchema.safeParse(body);
  if (!parsed.success) {
    return NextResponse.json(
      { error: "Invalid telemetry", issues: parsed.error.issues },
      { status: 400 },
    );
  }

  try {
    const redis = getRedis();
    const current = await redis.get<MessageRecord>(messageKey(parsed.data.id));
    if (!current)
      return NextResponse.json(
        { error: "Unknown message ID" },
        { status: 404 },
      );

    const updated: MessageRecord = {
      ...current,
      status: parsed.data.status,
      payload: parsed.data.output?.text ?? current.payload,
      updatedAt: Date.now(),
    };
    await redis.set(messageKey(parsed.data.id), {
      ...updated,
      telemetry: parsed.data,
    });

    return NextResponse.json({ record: updated });
  } catch (error) {
    console.error("Telemetry write failed", error);
    return NextResponse.json(
      { error: "Telemetry could not be stored" },
      { status: 503 },
    );
  }
}
