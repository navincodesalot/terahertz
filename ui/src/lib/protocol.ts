import { z } from "zod";

export const COMMAND_CHANNEL = "laser_commands" as const;
export const MESSAGE_KEY_PREFIX = "message:" as const;
export const HISTORY_KEY = "transmission_history" as const;
export const MAX_TRANSFER_BYTES = 250 * 1024;
export const IMAGE_CHUNK_BYTES = 1024;
export const UART_BAUD = 250000;
export const UART_FORMAT = "8N1";

export const sendTextSchema = z.object({
  type: z.literal("text").default("text"),
  payload: z
    .string()
    .min(1, "Payload must not be empty")
    .refine(
      (value) => !/[\n\r]/.test(value),
      "Payload must not contain line breaks",
    )
    .refine(
      (value) =>
        new TextEncoder().encode(value).byteLength <= MAX_TRANSFER_BYTES,
      "Payload must be 250 KB or smaller",
    ),
});

export const textCommandSchema = z.object({
  id: z.string().min(1),
  type: z.literal("text"),
  payload: z.string(),
  timestamp: z.number().int().nonnegative(),
  transport: z.literal("uart"),
  baud: z.literal(UART_BAUD),
  uart: z.literal(UART_FORMAT),
});

export const imageStartCommandSchema = z.object({
  id: z.string().min(1),
  type: z.literal("image_start"),
  totalBytes: z.number().int().positive().max(MAX_TRANSFER_BYTES),
  chunkBytes: z.number().int().positive(),
  chunkCount: z.number().int().positive(),
  mimeType: z.string().min(1),
  timestamp: z.number().int().nonnegative(),
  transport: z.literal("uart"),
  baud: z.literal(UART_BAUD),
  uart: z.literal(UART_FORMAT),
});

export const imageChunkCommandSchema = z.object({
  id: z.string().min(1),
  type: z.literal("image_chunk"),
  index: z.number().int().nonnegative(),
  data: z.string().min(1),
  timestamp: z.number().int().nonnegative(),
});

export const imageEndCommandSchema = z.object({
  id: z.string().min(1),
  type: z.literal("image_end"),
  sha256: z.string().length(64),
  timestamp: z.number().int().nonnegative(),
});

export const commandSchema = z.discriminatedUnion("type", [
  textCommandSchema,
  imageStartCommandSchema,
  imageChunkCommandSchema,
  imageEndCommandSchema,
]);

export const messageStatusSchema = z.enum([
  "queued",
  "published",
  "failed",
  "success",
]);

export const telemetrySchema = z.object({
  receivedAt: z.number().int().nonnegative().optional(),
  receivedBytes: z.number().int().nonnegative().optional(),
  receivedPayload: z.string().optional(),
  checksumPassed: z.boolean().optional(),
  chunksReceived: z.number().int().nonnegative().optional(),
  chunksExpected: z.number().int().nonnegative().optional(),
  sha256Passed: z.boolean().optional(),
  transmissionMs: z.number().int().nonnegative().optional(),
});

export const messageRecordSchema = z.object({
  id: z.string().min(1),
  type: z.enum(["text", "image"]),
  payload: z.string().optional(),
  fileName: z.string().optional(),
  mimeType: z.string().optional(),
  inputBytes: z.number().int().nonnegative(),
  status: messageStatusSchema,
  updatedAt: z.number().int().nonnegative(),
  timestamp: z.number().int().nonnegative(),
  telemetry: telemetrySchema.optional(),
  error: z.string().optional(),
});

export type TextCommand = z.infer<typeof textCommandSchema>;
export type Command = z.infer<typeof commandSchema>;
export type Telemetry = z.infer<typeof telemetrySchema>;
export type MessageRecord = z.infer<typeof messageRecordSchema>;

export function messageKey(id: string) {
  return `${MESSAGE_KEY_PREFIX}${id}`;
}
