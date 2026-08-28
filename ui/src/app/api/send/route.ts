import { createHash, randomUUID } from "node:crypto";

import { NextResponse } from "next/server";

import { getRedis } from "@/lib/redis";
import {
  COMMAND_CHANNEL,
  HISTORY_KEY,
  IMAGE_CHUNK_BYTES,
  MAX_IMAGE_BYTES,
  UART_BAUD,
  UART_FORMAT,
  messageKey,
  sendTextSchema,
  type Command,
  type MessageRecord,
} from "@/lib/protocol";

export const runtime = "nodejs";

// The sender blocks on Serial1.flush() after every frame, so it stops reading
// its Redis socket while the laser is busy and TCP flow control paces the
// transfer for us. Publishing is therefore a straight burst; the only reason
// to batch is Upstash's per-request size limit. Artificially delaying here
// throttled the link far below line rate and pushed large images past the
// serverless function's duration limit.
const IMAGE_PUBLISH_BATCH_SIZE = 64;

async function persistAndPublish(
  commandId: string,
  timestamp: number,
  record: MessageRecord,
  commands: Command[],
) {
  const redis = getRedis();
  await redis.set(messageKey(commandId), record);
  await redis.zadd(HISTORY_KEY, { score: timestamp, member: commandId });

  try {
    for (
      let start = 0;
      start < commands.length;
      start += IMAGE_PUBLISH_BATCH_SIZE
    ) {
      const pipeline = redis.pipeline();
      for (const command of commands.slice(
        start,
        start + IMAGE_PUBLISH_BATCH_SIZE,
      )) {
        pipeline.publish(COMMAND_CHANNEL, JSON.stringify(command));
      }
      await pipeline.exec();
    }
  } catch (error) {
    const failedRecord: MessageRecord = {
      ...record,
      status: "failed",
      updatedAt: Date.now(),
      error: "Command could not be published",
    };
    await redis.set(messageKey(commandId), failedRecord);
    console.error("Command publish failed", { id: commandId, error });
    return { failedRecord };
  }

  const publishedRecord: MessageRecord = {
    ...record,
    status: "published",
    updatedAt: Date.now(),
  };
  await redis.set(messageKey(commandId), publishedRecord);
  console.info("Command published", {
    channel: COMMAND_CHANNEL,
    id: commandId,
    type: record.type,
    commandCount: commands.length,
  });
  return { publishedRecord };
}

function invalid(error: string, issues?: unknown) {
  return NextResponse.json(
    { error, ...(issues ? { issues } : {}) },
    { status: 400 },
  );
}

async function createTextCommand(request: Request) {
  let body: unknown;
  try {
    body = await request.json();
  } catch {
    return invalid("Request body must be valid JSON");
  }

  const parsed = sendTextSchema.safeParse(body);
  if (!parsed.success)
    return invalid("Invalid text request", parsed.error.issues);

  const timestamp = Date.now();
  const id = `msg_${randomUUID()}`;
  const command: Command = {
    id,
    type: "text",
    payload: parsed.data.payload,
    timestamp,
    transport: "uart",
    baud: UART_BAUD,
    uart: UART_FORMAT,
  };
  const record: MessageRecord = {
    ...command,
    inputBytes: Buffer.byteLength(command.payload, "utf8"),
    status: "queued",
    updatedAt: timestamp,
  };
  const result = await persistAndPublish(id, timestamp, record, [command]);
  if ("failedRecord" in result && result.failedRecord) {
    const failedRecord = result.failedRecord;
    return NextResponse.json(
      { error: failedRecord.error, command: failedRecord },
      { status: 503 },
    );
  }
  return NextResponse.json(
    { command: result.publishedRecord, delivery: "published" },
    { status: 202 },
  );
}

async function createImageCommand(request: Request) {
  const form = await request.formData();
  const value = form.get("file");
  if (!(value instanceof File))
    return invalid("Multipart request must include a file field");
  if (!value.type.startsWith("image/"))
    return invalid("Only image files are supported");
  if (value.size < 1 || value.size > MAX_IMAGE_BYTES) {
    return invalid("Image must be between 1 byte and 500 KB");
  }

  const bytes = Buffer.from(await value.arrayBuffer());
  const timestamp = Date.now();
  const id = `msg_${randomUUID()}`;
  const chunkCount = Math.ceil(bytes.length / IMAGE_CHUNK_BYTES);
  const sha256 = createHash("sha256").update(bytes).digest("hex");
  const commands: Command[] = [
    {
      id,
      type: "image_start",
      totalBytes: bytes.length,
      chunkBytes: IMAGE_CHUNK_BYTES,
      transport: "uart",
      baud: UART_BAUD,
      uart: UART_FORMAT,
      chunkCount,
      mimeType: value.type,
      timestamp,
    },
  ];

  for (let index = 0; index < chunkCount; index += 1) {
    const start = index * IMAGE_CHUNK_BYTES;
    commands.push({
      id,
      type: "image_chunk",
      index,
      data: bytes.subarray(start, start + IMAGE_CHUNK_BYTES).toString("base64"),
      timestamp,
    });
  }
  commands.push({ id, type: "image_end", sha256, timestamp });

  const record: MessageRecord = {
    id,
    type: "image",
    fileName: value.name,
    mimeType: value.type,
    inputBytes: bytes.length,
    status: "queued",
    timestamp,
    updatedAt: timestamp,
  };
  const result = await persistAndPublish(id, timestamp, record, commands);
  if ("failedRecord" in result && result.failedRecord) {
    const failedRecord = result.failedRecord;
    return NextResponse.json(
      { error: failedRecord.error, command: failedRecord },
      { status: 503 },
    );
  }
  return NextResponse.json(
    {
      command: result.publishedRecord,
      delivery: "published",
      chunks: chunkCount,
    },
    { status: 202 },
  );
}

export async function POST(request: Request) {
  try {
    return request.headers
      .get("content-type")
      ?.startsWith("multipart/form-data")
      ? await createImageCommand(request)
      : await createTextCommand(request);
  } catch (error) {
    console.error("Command creation failed", error);
    return NextResponse.json(
      { error: "Command could not be created" },
      { status: 503 },
    );
  }
}
