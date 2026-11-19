import net from "node:net";

// Client verso il motore: il motore fa da server sulla socket (vedi
// src/control/midi_socket.h).
//
// I messaggi passano da una coda: all'avvio il motore puo' non essere ancora in
// ascolto, e scartare il primo CC significherebbe perdere sempre il primo tasto
// dopo ogni avvio. La coda e' limitata, cosi' un motore spento a lungo non fa
// crescere la memoria.
export class MidiSocket {
  private socket: net.Socket | null = null;
  private queue: number[][] = [];
  private lastAttempt = 0;
  private readonly maxQueue = 512;

  constructor(
    private readonly path: string,
    private readonly log: (message: string) => void = () => {},
  ) {}

  private connect(): void {
    const now = Date.now();
    if (now - this.lastAttempt < 1000) return;
    this.lastAttempt = now;

    const socket = net.createConnection(this.path);
    this.socket = socket;
    socket.on("connect", () => {
      this.log(`socket: connesso a ${this.path}`);
      this.flush();
    });
    socket.on("error", (err) => this.log(`socket: ${err.message}`));
    socket.on("close", () => {
      if (this.socket === socket) this.socket = null;
    });
  }

  private flush(): void {
    const socket = this.socket;
    if (!socket || socket.connecting || socket.destroyed) return;
    while (this.queue.length > 0) {
      socket.write(Buffer.from(this.queue[0]));
      this.queue.shift();
    }
  }

  send(bytes: number[]): void {
    this.queue.push(bytes);
    if (this.queue.length > this.maxQueue) this.queue.shift();
    if (!this.socket) this.connect();
    this.flush();
  }
}
