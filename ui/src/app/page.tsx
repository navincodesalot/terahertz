"use client";

import Image from "next/image";
import { useEffect, useMemo, useRef, useState } from "react";
import {
  Check,
  CircleAlert,
  ImagePlus,
  LoaderCircle,
  Radio,
  Send,
  Trash2,
  Upload,
} from "lucide-react";

import {
  AlertDialog,
  AlertDialogAction,
  AlertDialogCancel,
  AlertDialogContent,
  AlertDialogDescription,
  AlertDialogFooter,
  AlertDialogHeader,
  AlertDialogTitle,
  AlertDialogTrigger,
} from "@/components/ui/alert-dialog";
import { Badge } from "@/components/ui/badge";
import { Button } from "@/components/ui/button";
import {
  Card,
  CardContent,
  CardDescription,
  CardFooter,
  CardHeader,
  CardTitle,
} from "@/components/ui/card";

import {
  Field,
  FieldDescription,
  FieldGroup,
  FieldLabel,
} from "@/components/ui/field";

import { ScrollArea } from "@/components/ui/scroll-area";

import { Separator } from "@/components/ui/separator";
import {
  Table,
  TableBody,
  TableCell,
  TableHead,
  TableHeader,
  TableRow,
} from "@/components/ui/table";
import { Textarea } from "@/components/ui/textarea";
import { MAX_TRANSFER_BYTES } from "@/lib/protocol";
import { toast } from "sonner";

type MessageType = "text" | "image";
type Status = "idle" | "sending" | "published" | "success" | "failed";
type Telemetry = {
  receivedAt?: number;
  receivedBytes?: number;
  receivedPayload?: string;
  checksumPassed?: boolean;
  chunksReceived?: number;
  chunksExpected?: number;
  sha256Passed?: boolean;
  transmissionMs?: number;
  bitsPerSecond?: number;
};

type RecordItem = {
  id: string;
  type: MessageType;
  payload?: string;
  fileName?: string;
  mimeType?: string;
  inputBytes: number;
  status: "queued" | "published" | "failed" | "success";
  timestamp: number;
  updatedAt: number;
  telemetry?: Telemetry;
};

type SendResult = { command?: RecordItem; error?: string };

const statusCopy: Record<Status, string> = {
  idle: "Ready to transmit",
  sending: "Publishing command",
  published: "Published · waiting for receiver",
  success: "Transmission confirmed",
  failed: "Transmission failed",
};

function formatBytes(bytes: number) {
  if (bytes < 1024) return `${bytes} B`;
  return `${(bytes / 1024).toFixed(1)} KB`;
}

function formatTime(timestamp: number) {
  return new Intl.DateTimeFormat(undefined, {
    hour: "numeric",
    minute: "2-digit",
  }).format(timestamp);
}

function statusVariant(status: RecordItem["status"]) {
  if (status === "success") return "default" as const;
  if (status === "failed") return "destructive" as const;
  return "secondary" as const;
}

export default function HomePage() {
  const [type, setType] = useState<MessageType>("text");
  const [text, setText] = useState("");
  const [file, setFile] = useState<File | null>(null);
  const [records, setRecords] = useState<RecordItem[]>([]);
  const [status, setStatus] = useState<Status>("idle");
  const [notice, setNotice] = useState("");
  const [clearOpen, setClearOpen] = useState(false);
  const [clearing, setClearing] = useState(false);
  const [imagePreviewUrl, setImagePreviewUrl] = useState<string | null>(null);
  const imagePreviewRef = useRef<string | null>(null);

  const latestReceived = records.find((r) => r.telemetry !== undefined);
  const textBytes = new TextEncoder().encode(text).byteLength;
  const textTooLarge = textBytes > MAX_TRANSFER_BYTES;
  const textHasNewlines = /[\n\r]/.test(text);
  const canSend =
    type === "text"
      ? text.trim().length > 0 && !textTooLarge && !textHasNewlines
      : file !== null;
  const fileTooLarge = file !== null && file.size > MAX_TRANSFER_BYTES;
  const inputSummary = useMemo(() => {
    if (type === "image")
      return file
        ? `${file.name} · ${formatBytes(file.size)}`
        : "No image selected";
    return `${new TextEncoder().encode(text).length} bytes`;
  }, [file, text, type]);

  useEffect(() => {
    return () => {
      if (imagePreviewRef.current) {
        URL.revokeObjectURL(imagePreviewRef.current);
      }
    };
  }, []);

  useEffect(() => {
    let cancelled = false;

    const fetchHistory = async () => {
      try {
        const response = await fetch("/api/history", { cache: "no-store" });
        if (!response.ok || cancelled) return;
        const data = (await response.json()) as { records: RecordItem[] };
        if (!cancelled) setRecords(data.records);
      } catch {
        // network error — silently retry on next interval
      }
    };

    void fetchHistory();
    const interval = setInterval(() => void fetchHistory(), 5000);
    return () => {
      cancelled = true;
      clearInterval(interval);
    };
  }, []);

  async function submit() {
    if (!canSend || fileTooLarge || textTooLarge || textHasNewlines) return;
    setStatus("sending");
    setNotice("");
    toast.loading("Publishing command", {
      id: "transmission",
      description: "Sending the command to the persistent ESP32 subscriber.",
    });

    const request =
      type === "image"
        ? (() => {
            const form = new FormData();
            if (file) form.append("file", file);
            return fetch("/api/send", { method: "POST", body: form });
          })()
        : fetch("/api/send", {
            method: "POST",
            headers: { "content-type": "application/json" },
            body: JSON.stringify({ type: "text", payload: text }),
          });

    try {
      const response = await request;
      const data = (await response.json()) as SendResult;
      if (!response.ok || !data.command)
        throw new Error(data.error ?? "The command could not be published");
      setStatus("published");
      toast.success("Command published", {
        id: "transmission",
        description:
          "The optical sender is listening. Waiting for receiver telemetry.",
      });
      setRecords((current) => [
        data.command!,
        ...current.filter((item) => item.id !== data.command!.id),
      ]);
      if (type === "text") setText("");
      if (imagePreviewRef.current) {
        URL.revokeObjectURL(imagePreviewRef.current);
        imagePreviewRef.current = null;
      }
      setImagePreviewUrl(null);
      setFile(null);
    } catch (error) {
      setStatus("failed");
      toast.error("Command failed", {
        id: "transmission",
        description:
          error instanceof Error
            ? error.message
            : "The command could not be published",
      });
      setNotice(
        error instanceof Error
          ? error.message
          : "The command could not be published",
      );
    }
  }

  async function clearHistory() {
    setClearing(true);
    try {
      const response = await fetch("/api/history", { method: "DELETE" });
      if (!response.ok) throw new Error("History could not be deleted");
      setRecords([]);
      setClearOpen(false);
    } catch (error) {
      setNotice(
        error instanceof Error ? error.message : "History could not be deleted",
      );
    } finally {
      setClearing(false);
    }
  }

  return (
    <main className="bg-background text-foreground min-h-svh">
      <div className="mx-auto flex min-h-svh max-w-7xl flex-col gap-6 px-4 py-5 sm:px-8 sm:py-8">
        <header className="flex flex-col gap-4 sm:flex-row sm:items-center sm:justify-between">
          <div className="flex items-center gap-3">
            <div className="bg-primary text-primary-foreground flex size-11 items-center justify-center rounded-xl shadow-sm">
              <Radio />
            </div>
            <div>
              <p className="font-heading text-lg font-semibold tracking-[0.18em]">
                TERAHERTZ
              </p>
              <p className="text-muted-foreground text-xs tracking-[0.22em]">
                FREE-SPACE OPTICAL LINK
              </p>
            </div>
          </div>
          <Badge variant="outline" className="w-fit gap-2">
            <span className="bg-primary size-2 rounded-full" /> Cloud command
            link online
          </Badge>
        </header>

        <Separator />

        <section className="grid gap-6 lg:grid-cols-[minmax(0,1fr)_minmax(0,1fr)]">
          <Card>
            <CardHeader>
              <div className="flex items-start justify-between gap-4">
                <div>
                  <CardDescription className="font-mono text-xs tracking-[0.2em] uppercase">
                    01 / Send
                  </CardDescription>
                  <CardTitle className="mt-2 text-2xl">
                    Transmit a message
                  </CardTitle>
                </div>
                <Send className="text-muted-foreground" />
              </div>
              <CardDescription>
                Commands are published to the persistent ESP32-S3 subscriber
                over Redis.
              </CardDescription>
            </CardHeader>
            <CardContent>
              <FieldGroup>
                <Field>
                  <FieldLabel htmlFor="message-type">Message type</FieldLabel>
                  <div
                    className="grid grid-cols-2 gap-2"
                    role="group"
                    aria-label="Message type"
                  >
                    <Button
                      type="button"
                      variant={type === "text" ? "default" : "outline"}
                      aria-pressed={type === "text"}
                      onClick={() => {
                        setType("text");
                        setNotice("");
                      }}
                    >
                      Text message
                    </Button>
                    <Button
                      type="button"
                      variant={type === "image" ? "default" : "outline"}
                      aria-pressed={type === "image"}
                      onClick={() => {
                        setType("image");
                        setNotice("");
                      }}
                    >
                      Image · chunked binary
                    </Button>
                  </div>
                </Field>
                {type === "text" ? (
                  <Field>
                    <FieldLabel htmlFor="payload">Payload</FieldLabel>
                    <Textarea
                      id="payload"
                      value={text}
                      onChange={(event) => setText(event.target.value)}
                      placeholder="Enter a message to send through the optical link..."
                      rows={8}
                      className="max-h-80 resize-y overflow-y-auto"
                    />
                    <FieldDescription>
                      UTF-8 text · maximum 250 KB at the cloud boundary.
                    </FieldDescription>
                    {textHasNewlines && (
                      <p className="text-destructive text-sm">
                        Line breaks are not supported — the optical link uses
                        newline as a packet delimiter.
                      </p>
                    )}
                    {textTooLarge && (
                      <p className="text-destructive text-sm">
                        This message exceeds the 250 KB limit.
                      </p>
                    )}
                  </Field>
                ) : (
                  <Field>
                    <FieldLabel htmlFor="image-upload">
                      Image payload
                    </FieldLabel>
                    <label
                      htmlFor="image-upload"
                      className="bg-muted/30 hover:bg-muted/60 flex min-h-48 cursor-pointer flex-col items-center justify-center gap-3 overflow-hidden rounded-lg border border-dashed p-3 text-center transition-colors"
                    >
                      {imagePreviewUrl ? (
                        <ScrollArea className="max-h-64 w-full">
                          <Image
                            src={imagePreviewUrl}
                            alt={file?.name ?? "Selected image preview"}
                            width={640}
                            height={480}
                            unoptimized
                            className="mx-auto max-w-full rounded-md object-contain"
                          />
                        </ScrollArea>
                      ) : (
                        <ImagePlus className="text-primary size-8" />
                      )}
                      <span className="max-w-full truncate font-medium">
                        {file ? file.name : "Choose an image"}
                      </span>
                      <span className="text-muted-foreground text-sm">
                        PNG, JPEG, GIF or WebP · maximum 250 KB
                      </span>
                      <input
                        id="image-upload"
                        type="file"
                        accept="image/*"
                        className="sr-only"
                        onChange={(event) => {
                          if (imagePreviewRef.current) {
                            URL.revokeObjectURL(imagePreviewRef.current);
                            imagePreviewRef.current = null;
                          }
                          const selectedFile = event.target.files?.[0] ?? null;
                          const previewUrl = selectedFile
                            ? URL.createObjectURL(selectedFile)
                            : null;
                          imagePreviewRef.current = previewUrl;
                          setImagePreviewUrl(previewUrl);
                          setFile(selectedFile);
                        }}
                      />
                    </label>
                    <FieldDescription>
                      {file
                        ? `${formatBytes(file.size)} selected · sent as bounded UART chunks.`
                        : "The image is split into ordered chunks and checksummed."}
                    </FieldDescription>
                  </Field>
                )}
                {notice && (
                  <p className="text-destructive flex items-center gap-2 text-sm">
                    <CircleAlert className="size-4" />
                    {notice}
                  </p>
                )}
              </FieldGroup>
            </CardContent>
            <CardFooter className="flex-col items-stretch gap-4">
              {status !== "idle" && (
                <div className="flex items-center justify-between gap-3 rounded-lg border px-3 py-2">
                  <span className="text-sm">{statusCopy[status]}</span>
                  <Badge
                    variant={statusVariant(
                      status === "sending" ? "published" : status,
                    )}
                  >
                    {status === "sending" ? "working" : status}
                  </Badge>
                </div>
              )}
              <div className="text-muted-foreground flex items-center justify-between text-xs">
                <span>{inputSummary}</span>
                <span className="font-mono">UART · 0.25 Mbps · 8N1</span>
              </div>
              <Button
                size="lg"
                disabled={!canSend || fileTooLarge || status === "sending"}
                onClick={() => void submit()}
              >
                <Upload data-icon="inline-start" />
                {status === "sending"
                  ? "Publishing..."
                  : "Send through optical link"}
              </Button>
              {fileTooLarge && (
                <p className="text-destructive text-sm">
                  This file exceeds the 250 KB limit.
                </p>
              )}
            </CardFooter>
          </Card>

          <Card>
            <CardHeader>
              <div className="flex items-start justify-between gap-4">
                <div>
                  <CardDescription className="font-mono text-xs tracking-[0.2em] uppercase">
                    02 / Receive
                  </CardDescription>
                  <CardTitle className="mt-2 text-2xl">
                    Received message
                  </CardTitle>
                </div>
                <Badge
                  variant={
                    latestReceived
                      ? statusVariant(latestReceived.status)
                      : "outline"
                  }
                >
                  {latestReceived?.status ?? "waiting"}
                </Badge>
              </div>
              <CardDescription>
                Updated when the receiver ESP32 POSTs telemetry after each
                optical delivery.
              </CardDescription>
            </CardHeader>
            <CardContent className="flex flex-col gap-5">
              <div className="bg-muted/30 flex min-h-56 flex-col justify-between rounded-lg border p-5">
                <p className="text-muted-foreground font-mono text-xs tracking-[0.2em] uppercase">
                  Optical output
                </p>
                <ScrollArea className="mt-8 max-h-72">
                  {latestReceived?.type === "text" &&
                  latestReceived.telemetry?.receivedPayload ? (
                    <p className="pr-3 text-2xl leading-relaxed wrap-break-word whitespace-pre-wrap">
                      {latestReceived.telemetry.receivedPayload}
                    </p>
                  ) : latestReceived?.type === "image" ? (
                    <div className="flex flex-col gap-1">
                      <p className="text-2xl font-medium">
                        {latestReceived.mimeType ?? "image"}
                      </p>
                      <p className="text-muted-foreground text-sm">
                        {formatBytes(
                          latestReceived.telemetry?.receivedBytes ??
                            latestReceived.inputBytes,
                        )}
                        {" · "}
                        {latestReceived.telemetry?.chunksReceived ?? "?"}
                        {"/"}
                        {latestReceived.telemetry?.chunksExpected ?? "?"} chunks
                      </p>
                    </div>
                  ) : (
                    <p className="text-muted-foreground text-2xl">
                      Waiting for the receiver ESP32…
                    </p>
                  )}
                </ScrollArea>
                <p className="text-muted-foreground mt-8 flex items-center gap-2 text-sm">
                  {latestReceived?.telemetry?.receivedAt ? (
                    <>
                      <Check className="text-primary size-4" />
                      {formatBytes(
                        latestReceived.telemetry.receivedBytes ??
                          latestReceived.inputBytes,
                      )}{" "}
                      received · 
                      {formatTime(latestReceived.telemetry.receivedAt)}
                    </>
                  ) : (
                    "No confirmed optical transmission yet"
                  )}
                </p>
              </div>
              <div className="grid grid-cols-3 gap-3">
                <Card size="sm">
                  <CardContent className="p-3">
                    <p className="text-muted-foreground text-xs">Speed</p>
                    <p className="mt-1 font-mono font-medium">0.25 Mbps</p>
                  </CardContent>
                </Card>
                <Card size="sm">
                  <CardContent className="p-3">
                    <p className="text-muted-foreground text-xs">Duration</p>
                    <p className="mt-1 font-mono font-medium">
                      {latestReceived?.telemetry?.transmissionMs !== undefined
                        ? `${latestReceived.telemetry.transmissionMs} ms`
                        : "—"}
                    </p>
                  </CardContent>
                </Card>
                <Card size="sm">
                  <CardContent className="p-3">
                    <p className="text-muted-foreground text-xs">Integrity</p>
                    {latestReceived?.telemetry ? (
                      <Badge
                        variant={
                          (
                            latestReceived.type === "text"
                              ? latestReceived.telemetry.checksumPassed
                              : latestReceived.telemetry.sha256Passed
                          )
                            ? "default"
                            : "destructive"
                        }
                        className="mt-1"
                      >
                        {latestReceived.type === "text"
                          ? latestReceived.telemetry.checksumPassed
                            ? "CRC pass"
                            : "CRC fail"
                          : latestReceived.telemetry.sha256Passed
                            ? "SHA-256 pass"
                            : "SHA-256 fail"}
                      </Badge>
                    ) : (
                      <p className="text-muted-foreground mt-1 font-mono font-medium">
                        —
                      </p>
                    )}
                  </CardContent>
                </Card>
              </div>
            </CardContent>
          </Card>
        </section>

        <Card>
          <CardHeader>
            <div className="flex flex-col gap-4 sm:flex-row sm:items-start sm:justify-between">
              <div>
                <CardDescription className="font-mono text-xs tracking-[0.2em] uppercase">
                  03 / Log
                </CardDescription>
                <CardTitle className="mt-2 text-2xl">
                  Transmission history
                </CardTitle>
              </div>
              <AlertDialog open={clearOpen} onOpenChange={setClearOpen}>
                <AlertDialogTrigger
                  render={
                    <Button variant="outline" disabled={records.length === 0} />
                  }
                >
                  <Trash2 data-icon="inline-start" />
                  Clear history
                </AlertDialogTrigger>
                <AlertDialogContent>
                  <AlertDialogHeader>
                    <AlertDialogTitle>
                      Clear transmission history?
                    </AlertDialogTitle>
                    <AlertDialogDescription>
                      This permanently deletes every stored transmission record.
                      This action cannot be undone.
                    </AlertDialogDescription>
                  </AlertDialogHeader>
                  <AlertDialogFooter>
                    <AlertDialogCancel disabled={clearing}>
                      Cancel
                    </AlertDialogCancel>
                    <AlertDialogAction
                      disabled={clearing}
                      aria-busy={clearing}
                      onClick={(event) => {
                        event.preventDefault();
                        void clearHistory();
                      }}
                    >
                      {clearing ? (
                        <LoaderCircle
                          data-icon="inline-start"
                          className="animate-spin"
                        />
                      ) : (
                        <Trash2 data-icon="inline-start" />
                      )}
                      {clearing ? "Deleting..." : "Delete history"}
                    </AlertDialogAction>
                  </AlertDialogFooter>
                </AlertDialogContent>
              </AlertDialog>
            </div>
          </CardHeader>
          <CardContent>
            {records.length === 0 ? (
              <div className="flex flex-col items-center gap-2 rounded-lg border border-dashed p-10 text-center">
                <Radio className="text-muted-foreground" />
                <p className="font-medium">No transmissions recorded yet</p>
                <p className="text-muted-foreground text-sm">
                  Send a text or image to create the first command record.
                </p>
              </div>
            ) : (
              <div className="overflow-x-auto">
                <ScrollArea className="max-h-96">
                  <Table>
                    <TableHeader>
                      <TableRow>
                        <TableHead>Message</TableHead>
                        <TableHead>Type</TableHead>
                        <TableHead>Status</TableHead>
                        <TableHead>Size</TableHead>
                        <TableHead className="text-right">Time</TableHead>
                      </TableRow>
                    </TableHeader>
                    <TableBody>
                      {records.map((record) => (
                        <TableRow key={record.id}>
                          <TableCell className="max-w-64 truncate font-medium">
                            {record.type === "image"
                              ? record.fileName
                              : record.payload}
                          </TableCell>
                          <TableCell className="text-muted-foreground uppercase">
                            {record.type}
                          </TableCell>
                          <TableCell>
                            <Badge variant={statusVariant(record.status)}>
                              {record.status}
                            </Badge>
                          </TableCell>
                          <TableCell className="text-muted-foreground font-mono">
                            {formatBytes(record.inputBytes)}
                          </TableCell>
                          <TableCell className="text-muted-foreground text-right">
                            {formatTime(record.timestamp)}
                          </TableCell>
                        </TableRow>
                      ))}
                    </TableBody>
                  </Table>
                </ScrollArea>
              </div>
            )}
          </CardContent>
        </Card>
      </div>
    </main>
  );
}
