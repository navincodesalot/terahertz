import { NextResponse } from "next/server";

export const runtime = "nodejs";

export function POST() {
  return NextResponse.json(
    {
      error:
        "Receiver-to-browser integration is deferred; read receiver results from USB serial",
    },
    { status: 501 },
  );
}
