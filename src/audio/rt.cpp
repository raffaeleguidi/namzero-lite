#include "audio/rt.h"

#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>

#include <cerrno>
#include <cstring>

namespace namzero {
namespace rt {
namespace {

std::string sys_error(const char* what, int err) {
    std::string s(what);
    s += ": ";
    s += std::strerror(err);
    return s;
}

}  // namespace

bool lock_memory(std::string& err) {
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        err = sys_error("mlockall", errno);
        return false;
    }
    return true;
}

bool install(std::string& err) {
    bool ok = true;

    sched_param param{};
    param.sched_priority = 80;
    // pthread_* restituiscono il codice d'errore: errno non e' affidabile qui.
    int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
    if (rc != 0) {
        err = sys_error("pthread_setschedparam(SCHED_FIFO)", rc);
        ok = false;
    }

    std::string lock_err;
    if (!lock_memory(lock_err)) {
        if (ok) err = lock_err;
        ok = false;
    }
    return ok;
}

}  // namespace rt
}  // namespace namzero
