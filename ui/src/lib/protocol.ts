import { z } from "zod";

export const COMMAND_CHANNEL = "laser_commands" as const;
export const MESSAGE_KEY_PREFIX = "message:" as const;
export const HISTORY_KEY = "transmission_history" as const;
export const MAX_IMAGE_BYTES = 500 * 1024;
// A corrupted frame fails the whole image either way, so chunk size only
// trades per-frame overhead against RAM. 1 KB keeps framing overhead at ~1.5%
// while staying inside the sender's 2 KB optical payload limit.
export const IMAGE_CHUNK_BYTES = 1024;
export const UART_BAUD = 230400;
export const UART_FORMAT = "8N1";

const MAX_TEXT_BYTES = 2048;

export const sendTextSchema = z.object({
  type: z.literal("text").default("text"),
  payload: z
    .string()
    .min(1, "Payload must not be empty")
    // The sender measures the UTF-8 encoding, not the character count, and
    // silently drops anything past its optical payload limit.
    .refine(
      (value) => new TextEncoder().encode(value).length <= MAX_TEXT_BYTES,
      "Payload must be 2 KB or smaller once UTF-8 encoded",
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
  totalBytes: z.number().int().positive().max(MAX_IMAGE_BYTES),
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
  error: z.string().optional(),
});

export type TextCommand = z.infer<typeof textCommandSchema>;
export type Command = z.infer<typeof commandSchema>;
export type MessageRecord = z.infer<typeof messageRecordSchema>;

export function messageKey(id: string) {
  return `${MESSAGE_KEY_PREFIX}${id}`;
}
