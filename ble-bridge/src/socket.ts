import net from "node:net";

// Client verso il motore: il motore fa da server sulla socket (vedi
// src/control/midi_socket.h). Se il motore non c'e' la connessione fallisce e
// si riprova al messaggio successivo: il bridge non deve mai bloccare il BLE.
export class MidiSocket {
  private socket: net.Socket | null = null;
  private lastAttempt = 0;

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
    socket.on("connect", () => this.log(`socket: connesso a ${this.path}`));
    socket.on("error", (err) => this.log(`socket: ${err.message}`));
    socket.on("close", () => {
      if (this.socket === socket) this.socket = null;
    });
  }

  send(bytes: number[]): void {
    if (!this.socket) this.connect();
    if (!this.socket || this.socket.connecting || this.socket.destroyed) return;
    this.socket.write(Buffer.from(bytes));
  }
}
