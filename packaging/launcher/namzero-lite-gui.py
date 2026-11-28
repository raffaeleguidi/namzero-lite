#!/usr/bin/env python3
"""Launcher desktop di namzero-lite: GTK3 + WebKit2GTK, nessuna dipendenza pip.

Perche' esiste
--------------
Il motore apre config e file dei preset con percorsi *relativi alla propria
working directory*: "config/presets.json", "config/midi.json", "models/x.nam",
"irs/y.wav" (src/main.cpp). Nel pacchetto il motore sta in /opt/namzero-lite,
che e' di root: un utente normale non potrebbe ne' salvare le opzioni ne'
caricare preset. Il launcher ricostruisce quindi per-utente il layout che il
motore si aspetta e lo avvia con cwd = root dati:

    ~/.config/namzero/            config (copiata dalla factory al primo avvio)
    ~/.local/share/namzero/       root dati = cwd del motore
        config -> ~/.config/namzero
        models/
        irs/

Le pagine web restano condivise in /opt/namzero-lite/web: alla UI si passa
NAMZERO_ROOT=<root dati>, cosi' legge config/models/irs dalla root per-utente
mentre gli asset web non vengono duplicati.

Processi
--------
Motore e web server sono figli del launcher (subprocess.Popen) con stdout e
stderr in append su ~/.local/state/namzero/namzero-lite-gui.log: quando
qualcosa non parte c'e' un solo file da leggere, sia per l'utente sia per la
pagina di errore mostrata nella finestra. Alla chiusura i figli ricevono
SIGTERM e, dopo ~3 s, SIGKILL: un motore orfano terrebbe il lock
/tmp/namzero-lite.lock e il launcher successivo non potrebbe avviarne uno.
"""

from __future__ import annotations

import argparse
import fcntl
import html
import os
import shutil
import signal
import socket
import subprocess
import sys
import time

# ── posizioni nel pacchetto ────────────────────────────────────────────────

APP_DIR = "/opt/namzero-lite"
ENGINE = os.path.join(APP_DIR, "namzero-lite")
WEB_SERVER = os.path.join(APP_DIR, "web", "server.py")
SYSTEM_PYTHON = "/usr/bin/python3"
# Lock esclusivo del motore: se e' preso, un secondo motore uscirebbe subito.
ENGINE_LOCK = "/tmp/namzero-lite.lock"

HOST = "127.0.0.1"
DEFAULT_PORT = 8080
# Il motore deve aprire device e modelli: 10 s coprono un Pi Zero sotto carico.
WAIT_TIMEOUT_S = 10.0
POLL_MS = 200
TERM_TIMEOUT_S = 3.0
KILL_TIMEOUT_S = 1.0

# L'import di gi e' l'unico punto in cui la GUI puo' mancare del tutto: senza
# pygobject non c'e' nulla da fare, e un traceback non dice all'utente cosa
# installare. ValueError arriva da require_version quando il typelib esiste ma
# nella versione sbagliata (es. solo webkit2gtk-4.0).
try:
    import gi

    gi.require_version("Gtk", "3.0")
    gi.require_version("WebKit2", "4.1")
    from gi.repository import GLib, Gtk, WebKit2
except (ImportError, ValueError) as exc:  # pragma: no cover - dipende dal sistema
    print(
        "namzero-lite-gui: manca GTK3/WebKit2GTK per Python 3 (%s).\n"
        "Installare: sudo apt install python3-gi gir1.2-gtk-3.0 gir1.2-webkit2-4.1"
        % exc,
        file=sys.stderr,
    )
    raise SystemExit(1)


# ── directory XDG ──────────────────────────────────────────────────────────


def _xdg_dir(var: str, fallback: str) -> str:
    """Directory XDG: una variabile vuota o relativa vale come non impostata."""
    value = os.environ.get(var, "").strip()
    if not value or not os.path.isabs(value):
        value = os.path.expanduser(fallback)
    return value


def config_dir() -> str:
    return os.path.join(_xdg_dir("XDG_CONFIG_HOME", "~/.config"), "namzero")


def data_dir() -> str:
    return os.path.join(_xdg_dir("XDG_DATA_HOME", "~/.local/share"), "namzero")


def state_dir() -> str:
    return os.path.join(_xdg_dir("XDG_STATE_HOME", "~/.local/state"), "namzero")


# ── layout per-utente ──────────────────────────────────────────────────────


def factory_config_dirs() -> tuple[str, ...]:
    """Dove sta la config di fabbrica, in ordine di preferenza.

    /etc/namzero sono i conffiles del pacchetto; /opt/namzero-lite/config e'
    un symlink alla stessa directory e serve solo ai checkout git, dove /etc
    non e' stato scritto da nessun postinst.
    """
    return ("/etc/namzero", os.path.join(APP_DIR, "config"))


def ensure_config() -> tuple[str, bool]:
    """Crea ~/.config/namzero e la popola la prima volta.

    Una sorgente che risulta leggibile ma non copiabile (permessi, /etc
    smontato a meta', I/O error) non deve far crashare la GUI: si passa alla
    successiva e, se non resta nulla, si parte con la config vuota (il motore
    usa i suoi default). Si evita anche di copiare da una directory che *e'*
    gia' la destinazione, caso possibile se XDG_CONFIG_HOME punta dentro /etc.
    """
    cfg = config_dir()
    os.makedirs(cfg, exist_ok=True)
    if os.path.isfile(os.path.join(cfg, "presets.json")):
        return cfg, False
    target = os.path.realpath(cfg)
    for src in factory_config_dirs():
        try:
            if not os.path.isdir(src) or os.path.realpath(src) == target:
                continue
            shutil.copytree(src, cfg, dirs_exist_ok=True)
            return cfg, True
        except OSError:
            continue
    return cfg, False


def ensure_data_root(cfg: str) -> tuple[str, bool]:
    """Prepara la root dati (cwd del motore) e il symlink `config`.

    Il symlink evita di tenere due copie della config: il motore scrive
    .last_slot e i preset salvati in `config/...`, e quei file devono essere
    gli stessi che la web UI mostra. Ritorna (root, config_collegata).
    """
    data = data_dir()
    os.makedirs(os.path.join(data, "models"), exist_ok=True)
    os.makedirs(os.path.join(data, "irs"), exist_ok=True)
    link = os.path.join(data, "config")
    target = os.path.realpath(cfg)

    if os.path.islink(link):
        if os.path.realpath(link) != target:
            os.unlink(link)
            os.symlink(cfg, link)
        return data, True
    if os.path.isdir(link):
        # Directory vera al posto del symlink (install vecchio o esperimento
        # manuale): la sostituiamo solo se e' vuota. Non cancelliamo mai file
        # dell'utente per "aggiustare" il layout.
        try:
            os.rmdir(link)
        except OSError:
            return data, False
        os.symlink(cfg, link)
        return data, True
    if os.path.lexists(link):
        os.unlink(link)
    os.symlink(cfg, link)
    return data, True


def seed_models(data: str) -> int:
    """Primo avvio: copia i modelli di fabbrica nella root per-utente.

    I preset citano "models/xxx.nam" relativi alla cwd del motore: senza una
    copia locale il preset non trova l'amplificatore. Si copiano solo i .nam,
    solo se la directory e' ancora vuota: gli upload della UI restano intatti.
    """
    dst = os.path.join(data, "models")
    try:
        if any(n.endswith(".nam") for n in os.listdir(dst)):
            return 0
    except OSError:
        return 0
    src = os.path.join(APP_DIR, "models")
    if not os.path.isdir(src):
        return 0
    copied = 0
    for name in sorted(os.listdir(src)):
        if not name.endswith(".nam"):
            continue
        shutil.copy2(os.path.join(src, name), os.path.join(dst, name))
        copied += 1
    return copied


# ── rete e lock del motore ─────────────────────────────────────────────────


def resolve_port() -> int:
    """Porta della web UI: NAMZERO_WEB_PORT, altrimenti 8080 (come server.py)."""
    raw = os.environ.get("NAMZERO_WEB_PORT", "").strip()
    if not raw:
        return DEFAULT_PORT
    try:
        port = int(raw, 10)
    except ValueError:
        raise ValueError("NAMZERO_WEB_PORT non numerica: %r" % raw)
    if not 1 <= port <= 65535:
        raise ValueError("NAMZERO_WEB_PORT fuori intervallo: %d" % port)
    return port


def port_is_open(port: int) -> bool:
    """True se qualcuno e' in ascolto: si usa connect, non bind.

    Il bind direbbe solo "porta libera adesso"; il connect dice anche che il
    server gia' avviato risponde, che e' quello che serve alla WebView.
    """
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(0.25)
    try:
        return sock.connect_ex((HOST, port)) == 0
    except OSError:
        return False
    finally:
        sock.close()


def engine_running() -> bool:
    """True se un altro motore tiene /tmp/namzero-lite.lock.

    Il motore fa flock(LOCK_EX|LOCK_NB) sul file: proviamo lo stesso lock in
    modo non bloccante e lo rilasciamo subito (close). Se due launcher partono
    nello stesso istante la gara la vince un motore solo, e il secondo esce da
    solo come previsto; qui evitiamo solo il caso normale, cioe' il servizio
    systemd gia' attivo.
    """
    try:
        fd = os.open(ENGINE_LOCK, os.O_RDWR | os.O_CREAT, 0o666)
    except OSError:
        return False
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        return True
    finally:
        os.close(fd)  # il lock si libera con la chiusura
    return False


# ── pagine HTML inline ─────────────────────────────────────────────────────


def _page(body: str) -> str:
    """Involucro comune: tema scuro, niente risorse esterne (anche offline)."""
    return (
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>namzero-lite</title><style>"
        "html,body{height:100%;margin:0}"
        "body{display:flex;align-items:center;justify-content:center;"
        "background:#16181d;color:#e6e8eb;"
        "font:15px/1.6 system-ui,-apple-system,'Segoe UI',sans-serif}"
        ".box{max-width:640px;padding:28px 32px;text-align:left}"
        "h1{margin:0 0 8px;font-size:20px;font-weight:600}"
        "p{margin:8px 0}.muted{color:#9aa3ad;font-size:13px}"
        "code,pre{font-family:ui-monospace,Menlo,Consolas,monospace;font-size:12.5px}"
        "pre{background:#0f1115;border:1px solid #2a2f38;border-radius:8px;"
        "padding:10px 12px;overflow-x:auto;color:#cfd6de}"
        ".wave{color:#f0a33a;letter-spacing:6px;font-size:34px;margin-bottom:10px}"
        "</style></head><body><div class=\"box\">" + body + "</div></body></html>"
    )


def splash_html(port: int) -> str:
    return _page(
        "<div class=\"wave\">\u2581\u2583\u2585\u2587\u2585\u2583\u2581</div>"
        "<h1>avvio in corso</h1>"
        "<p>attendo la web UI su <code>%s:%d</code> (max %d s)&hellip;</p>"
        "<p class=\"muted\">il primo avvio copia config e modelli nella tua "
        "home e pu&ograve; richiedere qualche secondo.</p>"
        % (HOST, port, int(WAIT_TIMEOUT_S))
    )


def error_html(
    port: int, log_path: str, detail: str, engine_state: str, web_external: bool
) -> str:
    log = html.escape(log_path)
    server_note = (
        "<p class=\"muted\">La porta era gi&agrave; occupata: la finestra si "
        "&egrave; collegata al web server esistente.</p>"
        if web_external
        else ""
    )
    return _page(
        "<div class=\"wave\">\u2581\u2583\u2585\u2587\u2585\u2583\u2581</div>"
        "<h1>web UI non raggiungibile</h1>"
        "<p>%s</p>"
        "<p>Indirizzo: <code>http://%s:%d/</code><br>"
        "Motore: %s</p>"
        "%s"
        "<p>Log di motore e web server:</p><pre>%s</pre>"
        "<p class=\"muted\">Per capire cosa &egrave; successo:</p>"
        "<pre>tail -n 60 %s</pre>"
        "<p class=\"muted\">Avvio manuale (per isolare il problema):</p>"
        "<pre>cd %s && /opt/namzero-lite/namzero-lite\n"
        "NAMZERO_ROOT=%s /usr/bin/python3 /opt/namzero-lite/web/server.py</pre>"
        % (
            html.escape(detail),
            HOST,
            port,
            html.escape(engine_state),
            server_note,
            log,
            log,
            html.escape(data_dir()),
            html.escape(data_dir()),
        )
    )


# ── launcher ───────────────────────────────────────────────────────────────


class Launcher:
    """Finestra + processi figli. Un solo ciclo: Gtk.main() e GLib.timeout_add.

    Niente thread: ogni attesa (porta pronta, figli chiusi) e' un timeout sul
    main loop, cosi' la finestra resta disegnata anche mentre si aspetta.
    """

    def __init__(self, args):
        self.args = args
        self.port = resolve_port()
        self.url = "http://%s:%d/" % (HOST, self.port)
        self.log_path = os.path.join(state_dir(), "namzero-lite-gui.log")
        self.data = ""
        self.window = None
        self.view = None
        self.engine = None
        self.web = None
        self.web_external = False
        # Due scadenze separate: l'attesa della porta e la chiusura dei
        # figli non devono mai sovrascriversi (una finestra chiusa durante
        # l'avvio userebbe la scadenza sbagliata).
        self._wait_deadline = 0.0
        self._shutdown_deadline = 0.0
        self._children = []
        self._shutting_down = False
        self._force_killed = False
        self._warn_dialog = None

    # ── ciclo di vita ─────────────────────────────────────────────────────

    def run(self) -> None:
        os.makedirs(state_dir(), exist_ok=True)
        self._note("avvio: porta %d, log %s" % (self.port, self.log_path))

        cfg, seeded = ensure_config()
        if seeded:
            self._note("config per-utente creata in %s" % cfg)
        self.data, linked = ensure_data_root(cfg)
        if not linked:
            self._note(
                "ATTENZIONE: %s/config non e' un symlink a %s"
                % (self.data, cfg)
            )
        copied = seed_models(self.data)
        if copied:
            self._note(
                "%d modelli di fabbrica copiati in %s/models" % (copied, self.data)
            )

        self._build_window()
        self.window.show_all()
        # Gli handler vanno installati prima del primo figlio: un SIGTERM in
        # mezzo all'avvio deve comunque chiudere quello che e' gia' partito.
        self._install_signal_handlers()

        if port_is_open(self.port):
            # Un web server risponde gia' (servizio systemd o un'altra GUI):
            # avviarne un secondo fallirebbe sul bind e mostrerebbe una UI
            # identica, quindi ci si collega a quello.
            self.web_external = True
            self._note(
                "porta %d gia' in ascolto: uso il web server esistente" % self.port
            )
        else:
            self._start_web()

        if self.args.no_engine:
            self._note("--no-engine: motore non avviato")
        elif engine_running():
            self._note(
                "motore gia' attivo (lock %s): non ne avvio un secondo" % ENGINE_LOCK
            )
            self._warn_engine_running()
        else:
            self._start_engine()

        self._wait_deadline = time.monotonic() + WAIT_TIMEOUT_S
        GLib.timeout_add(POLL_MS, self._poll_ready)
        Gtk.main()
        self._note("uscita")

    def _build_window(self) -> None:
        window = Gtk.Window()
        window.set_title("namzero-lite")
        window.set_default_size(1100, 800)
        # Nome dell'icona, non un file: l'icona e' installata in hicolor come
        # namzero-lite.svg, cosi' il tema puo' fornirne una sua.
        window.set_icon_name("namzero-lite")
        window.set_position(Gtk.WindowPosition.CENTER)
        window.connect("destroy", self._on_destroy)

        self.view = WebKit2.WebView()
        if self.args.debug:
            # Ispettore e console servono solo a chi passa --debug.
            self.view.get_settings().set_enable_developer_extras(True)
        window.add(self.view)
        self.window = window
        self.view.load_html(splash_html(self.port), "file:///")

    # ── figli ─────────────────────────────────────────────────────────────

    def _spawn(self, cmd: list[str], env: dict, cwd: str, label: str):
        """Avvia un figlio con stdout/stderr sullo stesso log in append.

        start_new_session isola il figlio dal nostro process group: un Ctrl-C
        nel terminale arriva solo al launcher, che chiude i figli in ordine
        (SIGTERM, poi SIGKILL) invece di lasciarli a meta' inizializzazione.
        """
        with open(self.log_path, "a", encoding="utf-8", buffering=1) as log:
            log.write(
                "\n=== %s %s: %s (cwd=%s)\n"
                % (time.strftime("%Y-%m-%d %H:%M:%S"), label, " ".join(cmd), cwd)
            )
            log.flush()
            try:
                proc = subprocess.Popen(
                    cmd,
                    cwd=cwd,
                    env=env,
                    stdin=subprocess.DEVNULL,
                    stdout=log,
                    stderr=subprocess.STDOUT,
                    start_new_session=True,
                    close_fds=True,
                )
            except OSError as exc:
                self._note("avvio di %s fallito: %s" % (label, exc))
                return None
        self._note("%s avviato (pid %d)" % (label, proc.pid))
        return proc

    def _start_engine(self) -> None:
        if not os.access(ENGINE, os.X_OK):
            self._note("ATTENZIONE: motore assente o non eseguibile: %s" % ENGINE)
            return
        # Il motore non accetta un --root: la cwd *e'* la root dati, e i
        # percorsi relativi dei preset funzionano solo da li'.
        self.engine = self._spawn([ENGINE], os.environ.copy(), self.data, "motore")

    def _start_web(self) -> None:
        if not os.path.isfile(WEB_SERVER):
            self._note("ATTENZIONE: web server assente: %s" % WEB_SERVER)
            return
        python = SYSTEM_PYTHON if os.access(SYSTEM_PYTHON, os.X_OK) else sys.executable
        env = os.environ.copy()
        # La porta va passata esplicitamente: il server ha un default suo
        # (8080) e deve ascoltare esattamente dove si collega la WebView.
        env["NAMZERO_WEB_PORT"] = str(self.port)
        # NAMZERO_ROOT sposta config/models/irs sulla root per-utente; gli
        # asset web restano nella directory del pacchetto.
        env["NAMZERO_ROOT"] = self.data
        # Senza questo il log della UI arriverebbe a blocchi da 4 KB: nella
        # pagina di errore si leggerebbe solo roba vecchia.
        env["PYTHONUNBUFFERED"] = "1"
        self.web = self._spawn([python, WEB_SERVER], env, APP_DIR, "web server")

    def _warn_engine_running(self) -> None:
        """Avviso non modale: il launcher non deve restare appeso.

        run() su questo dialogo aprirebbe un loop GTK annidato prima di
        Gtk.main(): un SIGTERM in quel momento resterebbe in coda e il web
        server gia' avviato diventerebbe orfano. show_all() mostra lo stesso
        messaggio e lascia il controllo al loop principale.
        """
        dialog = Gtk.MessageDialog(
            transient_for=self.window,
            modal=False,
            message_type=Gtk.MessageType.WARNING,
            buttons=Gtk.ButtonsType.OK,
            text="namzero-lite \u00e8 gi\u00e0 in esecuzione",
        )
        dialog.format_secondary_text(
            "Un altro motore tiene il lock %s, quindi non ne avvio un secondo.\n\n"
            "La finestra si collega alla web UI attiva; per fermare il motore "
            "usa \u00absudo systemctl stop namzero-lite\u00bb." % ENGINE_LOCK
        )
        self._warn_dialog = dialog
        dialog.show_all()

    # ── attesa della porta ────────────────────────────────────────────────

    def _poll_ready(self) -> bool:
        if self._shutting_down:
            return GLib.SOURCE_REMOVE
        if (
            self.web is not None
            and self.web.poll() is not None
            and not port_is_open(self.port)
        ):
            # Uscito prima di ascoltare: aspettare i 10 s non aggiungerebbe
            # nulla, il log dice gia' perche'.
            self._show_error("il web server \u00e8 uscito subito dopo l'avvio")
            return GLib.SOURCE_REMOVE
        if port_is_open(self.port):
            self.view.load_uri(self.url)
            self._note("web UI pronta su %s" % self.url)
            return GLib.SOURCE_REMOVE
        # Un motore uscito NON e' un errore: puo' essere la modalita' offline
        # (device audio assente) o un secondo motore che ha perso il lock.
        # La web UI funziona lo stesso, quindi si continua ad aspettare.
        if time.monotonic() >= self._wait_deadline:
            self._show_error(
                "la web UI non ha risposto entro %d s" % int(WAIT_TIMEOUT_S)
            )
            return GLib.SOURCE_REMOVE
        return GLib.SOURCE_CONTINUE

    def _show_error(self, detail: str) -> None:
        self._note("ERRORE: " + detail)
        engine_state = "non avviato (--no-engine)"
        if self.engine is not None:
            code = self.engine.poll()
            if code is None:
                engine_state = "in esecuzione (pid %d)" % self.engine.pid
            else:
                engine_state = (
                    "uscito con codice %s (possibile modalit\u00e0 offline: "
                    "controlla il device audio)" % code
                )
        self.view.load_html(
            error_html(
                self.port,
                self.log_path,
                detail,
                engine_state,
                self.web_external,
            ),
            "file:///",
        )

    # ── segnali e chiusura ────────────────────────────────────────────────

    def _install_signal_handlers(self) -> None:
        # Gtk.main() gira in C: un handler Python "normale" verrebbe eseguito
        # solo al prossimo bytecode Python, cioe' chissa' quando.
        # GLib.unix_signal_add aggancia il segnale al main loop, che gira
        # sempre, e il callback parte subito.
        for sig in (signal.SIGINT, signal.SIGTERM):
            GLib.unix_signal_add(GLib.PRIORITY_HIGH, sig, self._on_unix_signal)

    def _on_unix_signal(self) -> bool:
        self._note("segnale ricevuto: chiudo")
        if self.window is not None:
            self.window.destroy()  # destroy -> _begin_shutdown
        else:
            self._begin_shutdown()
        return GLib.SOURCE_CONTINUE

    def _on_destroy(self, *_args) -> None:
        self._begin_shutdown()

    def _begin_shutdown(self) -> None:
        if self._shutting_down:
            return
        self._shutting_down = True
        self._children = [p for p in (self.engine, self.web) if p is not None]
        for proc in self._children:
            if proc.poll() is None:
                try:
                    proc.terminate()  # SIGTERM: il motore salva ed esce pulito
                except OSError:
                    pass
        self._shutdown_deadline = time.monotonic() + TERM_TIMEOUT_S
        if not self._children:
            Gtk.main_quit()
            return
        GLib.timeout_add(100, self._poll_shutdown)

    def _poll_shutdown(self) -> bool:
        alive = [p for p in self._children if p.poll() is None]
        if not alive:
            Gtk.main_quit()
            return GLib.SOURCE_REMOVE
        now = time.monotonic()
        if now < self._shutdown_deadline:
            return GLib.SOURCE_CONTINUE
        if not self._force_killed:
            # SIGTERM ignorato (o motore bloccato su un device audio che non
            # risponde): dopo 3 s si passa a SIGKILL, altrimenti il lock
            # /tmp resta preso e il prossimo avvio non parte.
            for proc in alive:
                try:
                    proc.kill()
                except OSError:
                    pass
            self._force_killed = True
            self._shutdown_deadline = now + KILL_TIMEOUT_S
            return GLib.SOURCE_CONTINUE
        # Nemmeno SIGKILL ha liberato i figli in tempo: non si resta appesi.
        Gtk.main_quit()
        return GLib.SOURCE_REMOVE

    def force_kill(self) -> None:
        """Uscita di emergenza (es. Ctrl-C durante l'avvio, loop non ancora su)."""
        for proc in (self.engine, self.web):
            if proc is not None and proc.poll() is None:
                try:
                    proc.kill()
                except OSError:
                    pass

    # ── log ───────────────────────────────────────────────────────────────

    def _note(self, text: str) -> None:
        line = "%s launcher: %s" % (time.strftime("%Y-%m-%d %H:%M:%S"), text)
        try:
            with open(self.log_path, "a", encoding="utf-8") as fh:
                fh.write(line + "\n")
        except OSError:
            pass
        if self.args.debug:
            print("[namzero-lite-gui] " + text, file=sys.stderr, flush=True)


# ── avvio ──────────────────────────────────────────────────────────────────


def parse_args(argv=None):
    parser = argparse.ArgumentParser(
        prog="namzero-lite-gui",
        description="Finestra desktop per namzero-lite (motore + web UI locale).",
    )
    parser.add_argument(
        "--no-engine",
        action="store_true",
        help="non avviare il motore: usa quello gia' attivo "
        "(es. il servizio systemd namzero-lite)",
    )
    parser.add_argument(
        "--debug",
        action="store_true",
        help="log dettagliato su stderr e console WebKit abilitata",
    )
    return parser.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)
    # prgname prima della finestra: da qui GTK ricava il WM_CLASS, che deve
    # combaciare con StartupWMClass del file .desktop e col nome dell'icona.
    GLib.set_prgname("namzero-lite")
    GLib.set_application_name("namzero-lite")

    try:
        launcher = Launcher(args)
    except ValueError as exc:
        print("namzero-lite-gui: %s" % exc, file=sys.stderr)
        return 2
    try:
        launcher.run()
    except KeyboardInterrupt:
        # Arriva solo se il segnale non e' passato da GLib (avvio iniziale).
        launcher.force_kill()
        return 130
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
