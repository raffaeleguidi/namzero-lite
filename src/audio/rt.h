#pragma once

#include <string>

namespace namzero {
namespace rt {

// Imposta priorita' SCHED_FIFO sul thread corrente e blocca la memoria.
// Da chiamare all'inizio del thread audio, prima del loop.
bool install(std::string& err);

// Solo mlockall, utile per il thread di controllo.
bool lock_memory(std::string& err);

}  // namespace rt
}  // namespace namzero
