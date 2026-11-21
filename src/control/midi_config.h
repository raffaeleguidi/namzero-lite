#pragma once

#include <array>
#include <vector>
#include <cstddef>
#include <string>

#include "json.hpp"

#include "control/delay_modes.h"
#include "control/drive_types.h"
#include "control/mod_types.h"

namespace namzero {

// Azioni comandabili da un CC. L'ordine e' quello degli slot nella catena:
// serve a indicizzare gli array di stato e i flag di bypass.
enum class MidiAction {
    kGate = 0,
    kComp,
    kDrive,
    kMod,
    kAmp,
    kIr,
    kDelay,
    kReverb,
    kCount,
};

constexpr std::size_t kMidiActionCount = static_cast<std::size_t>(MidiAction::kCount);

inline const char* midi_action_name(MidiAction a) {
    switch (a) {
        case MidiAction::kGate: return "gate";
        case MidiAction::kComp: return "comp";
        case MidiAction::kDrive: return "drive";
        case MidiAction::kMod: return "mod";
        case MidiAction::kAmp: return "amp";
        case MidiAction::kIr: return "ir";
        case MidiAction::kDelay: return "delay";
        case MidiAction::kReverb: return "reverb";
        default: return "?";
    }
}

// Tutta la configurazione MIDI, uguale per ogni preset: sta in un file a parte
// (midi.json) proprio perche' cambiarla non deve richiedere ne' un preset nuovo
// ne' una ricompilazione.
struct MidiConfig {
    bool enabled = true;
    std::string device = "hw:XTONE";
    // Ingressi da aprire: se vuoto si usa `device`. Con piu' pedali USB si
    // elencano tutti qui.
    std::vector<std::string> devices;
    // L'XTONE manda l'espressione su CC diversi a seconda del gruppo (7 verde,
    // 4 blu, 1 rosso): si ascoltano tutti.
    std::vector<int> expression_ccs = {7};
    // I footswitch dell'XTONE sono "a stato" (un messaggio per pressione, valore
    // che alterna 127/0). "edge" serve ai controller momentanei.
    std::string switches = "any";

    int expression_cc = 7;
    int expression_min_raw = 0;
    int expression_max_raw = 127;
    bool expression_invert = false;

    double wah_threshold = 0.10;
    double wah_release_threshold = 0.05;
    int wah_engage_ms = 100;
    int wah_release_ms = 500;
    // true = la punta e' la zona spenta (wah acceso verso il tacco).
    bool wah_invert = false;

    // Un CC per azione; -1 = nessun binding. Di default restano legati solo i
    // tre storici, cosi' senza file il comportamento non cambia.
    std::array<int, kMidiActionCount> slot_cc = {-1, -1, 20, 10, -1, -1, 11, -1};

    // CC che SELEZIONANO un tipo di modulazione (un tasto per effetto), indicizzati
    // come kModTypeNames. Il wah non c'e' piu' fra i tipi di mod: vive nel
    // blocco filter e si accende solo dal pedale.
    std::array<int, kModTypeCount> mod_select_cc = {-1, -1, -1, -1};

    // Stessa cosa per il drive: un tasto per modello.
    std::array<int, kDriveTypeCount> drive_select_cc = {-1, -1, -1};

    // Stessa cosa per il delay: un tasto per modo.
    std::array<int, kDelayModeCount> delay_select_cc = {-1, -1, -1};

    // Boost di volume a fine catena, comandato da un CC: valore ASSOLUTO in dB
    // sul gain master (default +6 dB). cc < 0 = disattivato.
    int boost_cc = -1;
    double boost_db = 6.0;

    // G-Board: i suoi pulsanti mandano Program Change; qui si dice quale PC
    // seleziona quale effetto e su quale device pilotare i LED (Note On).
    std::string gboard_device;              // vuoto = nessun LED
    std::array<int, 4> gboard_mod_pc = {-1, -1, -1, -1};    // tremolo/univibe/flanger/phaser
    std::array<int, 3> gboard_drive_pc = {-1, -1, -1};      // fuzz/klon/tubescreamer
    int gboard_boost_pc = -1;               // PC del tasto boost (on/off)
    int gboard_flash_ms = 150;

    // Looper: CC del gruppo rosso dell'XTONE (30 toggle, 43 stop, 42 clear).
    int looper_toggle_cc = -1;
    int looper_stop_cc = -1;
    int looper_clear_cc = -1;

    // Percorso della socket del bridge BLE; vuoto = nessun bridge.
    std::string ble_socket;
};

// false = il file esiste ma non e' valido; err dice cosa. Un file assente non
// e' un errore: si chiama il loader solo se il file c'e'.
bool parse_midi_config(const nlohmann::json& j, MidiConfig& cfg, std::string& err);
bool load_midi_config(const std::string& path, MidiConfig& cfg, std::string& err);

}  // namespace namzero
