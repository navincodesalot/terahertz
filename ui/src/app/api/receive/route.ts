import { NextResponse } from "next/server";
import { z } from "zod";

import { getRedis } from "@/lib/redis";
import { messageKey, type MessageRecord } from "@/lib/protocol";

export const runtime = "nodejs";

const receiveSchema = z.object({
  id: z.string().min(1),
  status: z.enum(["success", "failed"]),
  type: z.enum(["text", "image"]),
  receivedPayload: z.string().optional(),
  receivedBytes: z.number().int().nonnegative().optional(),
  checksumPassed: z.boolean().optional(),
  chunksReceived: z.number().int().nonnegative().optional(),
  chunksExpected: z.number().int().nonnegative().optional(),
  sha256Passed: z.boolean().optional(),
  transmissionMs: z.number().int().nonnegative().optional(),
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

  const parsed = receiveSchema.safeParse(body);
  if (!parsed.success) {
    return NextResponse.json(
      { error: "Invalid telemetry report", issues: parsed.error.issues },
      { status: 400 },
    );
  }

  try {
    const redis = getRedis();
    const current = await redis.get<MessageRecord>(messageKey(parsed.data.id));
    if (!current) {
      return NextResponse.json(
        { error: "Unknown message ID" },
        { status: 404 },
      );
    }

    const updated: MessageRecord = {
      ...current,
      status: parsed.data.status,
      updatedAt: Date.now(),
      telemetry: {
        receivedAt: Date.now(),
        receivedBytes: parsed.data.receivedBytes,
        receivedPayload: parsed.data.receivedPayload,
        checksumPassed: parsed.data.checksumPassed,
        chunksReceived: parsed.data.chunksReceived,
        chunksExpected: parsed.data.chunksExpected,
        sha256Passed: parsed.data.sha256Passed,
        transmissionMs: parsed.data.transmissionMs,
      },
    };

    await redis.set(messageKey(parsed.data.id), updated);
    console.info("Telemetry stored", {
      id: parsed.data.id,
      status: parsed.data.status,
      type: parsed.data.type,
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
