import { cn } from "@/lib/utils";

function ScrollArea({ className, ...props }: React.ComponentProps<"div">) {
  return (
    <div
      data-slot="scroll-area"
      className={cn("relative overflow-auto", className)}
      {...props}
    />
  );
}

export { ScrollArea };
