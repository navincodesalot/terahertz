"use client";

import { useEffect, useMemo, useState } from "react";
import {
  Check,
  CircleAlert,
  ImagePlus,
  Radio,
  Send,
  Trash2,
  Upload,
  X,
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
  Dialog,
  DialogClose,
  DialogContent,
  DialogDescription,
  DialogHeader,
  DialogTitle,
} from "@/components/ui/dialog";
import {
  Field,
  FieldDescription,
  FieldGroup,
  FieldLabel,
} from "@/components/ui/field";
import { Progress } from "@/components/ui/progress";
import { toast } from "sonner";

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

const MAX_IMAGE_BYTES = 500 * 1024;

type MessageType = "text" | "image";
type Status = "idle" | "sending" | "published" | "success" | "failed";
type RecordItem = {
  id: string;
  type: MessageType;
  payload?: string;
  fileName?: string;
  inputBytes: number;
  status: "queued" | "published" | "failed" | "success";
  timestamp: number;
  updatedAt: number;
};

type SendResult = { command?: RecordItem; error?: string };

const statusCopy: Record<Status, string> = {
  idle: "Ready to transmit",
  sending: "Publishing command",
  published: "Awaiting receiver",
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
  const [popupOpen, setPopupOpen] = useState(false);
  const [clearDialogOpen, setClearDialogOpen] = useState(false);
  const [clearingHistory, setClearingHistory] = useState(false);

  const latest = records[0];
  const canSend = type === "text" ? text.trim().length > 0 : file !== null;
  const fileTooLarge = file !== null && file.size > MAX_IMAGE_BYTES;
  const inputSummary = useMemo(() => {
    if (type === "image")
      return file
        ? `${file.name} · ${formatBytes(file.size)}`
        : "No image selected";
    return `${new TextEncoder().encode(text).length} bytes`;
  }, [file, text, type]);

  useEffect(() => {
    let cancelled = false;
    void fetch("/api/history", { cache: "no-store" })
      .then(async (response) => {
        if (!response.ok) return;
        const data = (await response.json()) as { records: RecordItem[] };
        if (!cancelled) setRecords(data.records);
      })
      .catch(() => undefined);
    return () => {
      cancelled = true;
    };
  }, []);

  async function submit() {
    if (!canSend || fileTooLarge) return;
    setStatus("sending");
    setNotice("");
    setPopupOpen(true);

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
      setRecords((current) => [
        data.command!,
        ...current.filter((item) => item.id !== data.command!.id),
      ]);
      if (type === "text") setText("");
      setFile(null);
    } catch (error) {
      setStatus("failed");
      setNotice(
        error instanceof Error
          ? error.message
          : "The command could not be published",
      );
    }
  }

  async function clearHistory() {
    setClearingHistory(true);
    try {
      const response = await fetch("/api/history", { method: "DELETE" });
      if (!response.ok) {
        const data = (await response.json().catch(() => null)) as {
          error?: string;
        } | null;
        throw new Error(data?.error ?? "History could not be cleared");
      }

      setRecords([]);
      setClearDialogOpen(false);
      toast.success("Transmission history deleted");
    } catch (error) {
      toast.error(
        error instanceof Error ? error.message : "History could not be cleared",
      );
    } finally {
      setClearingHistory(false);
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
                DIY FSO TERAHERTZ
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
                    />
                    <FieldDescription>
                      UTF-8 text · maximum 32 KB at the cloud boundary.
                    </FieldDescription>
                  </Field>
                ) : (
                  <Field>
                    <FieldLabel htmlFor="image-upload">
                      Image payload
                    </FieldLabel>
                    <label
                      htmlFor="image-upload"
                      className="bg-muted/30 hover:bg-muted/60 flex min-h-48 cursor-pointer flex-col items-center justify-center gap-3 rounded-lg border border-dashed p-6 text-center transition-colors"
                    >
                      <ImagePlus className="text-primary size-8" />
                      <span className="font-medium">
                        {file ? file.name : "Choose an image"}
                      </span>
                      <span className="text-muted-foreground text-sm">
                        PNG, JPEG, GIF or WebP · maximum 500 KB
                      </span>
                      <input
                        id="image-upload"
                        type="file"
                        accept="image/*"
                        className="sr-only"
                        onChange={(event) =>
                          setFile(event.target.files?.[0] ?? null)
                        }
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
              <div className="text-muted-foreground flex items-center justify-between text-xs">
                <span>{inputSummary}</span>
                <span className="font-mono">UART · 250000 · 8N1</span>
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
                  This image exceeds the 500 KB limit.
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
                  variant={latest ? statusVariant(latest.status) : "outline"}
                >
                  {latest?.status ?? "waiting"}
                </Badge>
              </div>
              <CardDescription>
                The receiver panel will update when the physical ESP32 reports
                telemetry.
              </CardDescription>
            </CardHeader>
            <CardContent className="flex flex-col gap-5">
              <div className="bg-muted/30 flex min-h-56 flex-col justify-between rounded-lg border p-5">
                <p className="text-muted-foreground font-mono text-xs tracking-[0.2em] uppercase">
                  Optical output
                </p>
                <p className="mt-8 text-2xl leading-relaxed break-words">
                  {latest?.payload ?? "Waiting for the receiver ESP32..."}
                </p>
                <p className="text-muted-foreground mt-8 flex items-center gap-2 text-sm">
                  {latest ? (
                    <>
                      <Check className="text-primary size-4" />
                      {formatBytes(latest.inputBytes)} received ·{" "}
                      {formatTime(latest.updatedAt)}
                    </>
                  ) : (
                    "No confirmed optical transmission yet"
                  )}
                </p>
              </div>
              <div className="grid grid-cols-3 gap-3">
                <Card size="sm">
                  <CardContent className="p-3">
                    <p className="text-muted-foreground text-xs">Baud</p>
                    <p className="mt-1 font-mono font-medium">250000</p>
                  </CardContent>
                </Card>
                <Card size="sm">
                  <CardContent className="p-3">
                    <p className="text-muted-foreground text-xs">Format</p>
                    <p className="mt-1 font-mono font-medium">8N1</p>
                  </CardContent>
                </Card>
                <Card size="sm">
                  <CardContent className="p-3">
                    <p className="text-muted-foreground text-xs">Errors</p>
                    <p className="mt-1 font-mono font-medium">—</p>
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
              <AlertDialog
                open={clearDialogOpen}
                onOpenChange={setClearDialogOpen}
              >
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
                    <AlertDialogCancel>Cancel</AlertDialogCancel>
                    <AlertDialogAction
                      disabled={clearingHistory}
                      onClick={(event) => {
                        event.preventDefault();
                        void clearHistory();
                      }}
                    >
                      {clearingHistory ? "Deleting…" : "Delete history"}
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
              </div>
            )}
          </CardContent>
        </Card>
      </div>

      <Dialog open={popupOpen} onOpenChange={setPopupOpen}>
        <DialogContent>
          <DialogHeader>
            <DialogTitle className="flex items-center gap-3">
              <span className="bg-primary text-primary-foreground flex size-9 items-center justify-center rounded-full">
                <Radio
                  className={status === "sending" ? "animate-pulse" : ""}
                />
              </span>
              {statusCopy[status]}
            </DialogTitle>
            <DialogDescription>
              {status === "published"
                ? "The command is live on Redis Pub/Sub. The receiver will confirm the optical checksum when telemetry is connected."
                : status === "failed"
                  ? notice
                  : "Preparing the command for the persistent optical link."}
            </DialogDescription>
          </DialogHeader>
          <Progress
            value={
              status === "sending"
                ? 30
                : status === "published"
                  ? 65
                  : status === "success"
                    ? 100
                    : 0
            }
          />
          <div className="text-muted-foreground flex justify-between text-xs">
            <span>Queued</span>
            <span>Published</span>
            <span>Verified</span>
          </div>
          <DialogClose render={<Button variant="outline" className="w-full" />}>
            <X data-icon="inline-start" />
            Close
          </DialogClose>
        </DialogContent>
      </Dialog>
    </main>
  );
}
