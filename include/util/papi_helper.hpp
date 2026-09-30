#if defined(ENABLE_PAPI) && !defined(PAPICOUNTER_HPP)
#define PAPICOUNTER_HPP

#include <papi.h>
#include <string>
#include <iostream>
#include <stdexcept>

namespace papi {

class IPapiCounter {
    public:
        virtual void start() = 0;
        virtual void stop() = 0;
        virtual void write(std::ostream & out) const = 0;
};

template <unsigned N>
class PapiCounter : public IPapiCounter
{
    typedef long_long papi_counter_t;

public:

    inline PapiCounter(std::initializer_list<int> events);
    inline ~PapiCounter();

    virtual void start();
    virtual void stop();
    virtual void write(std::ostream & out) const;

private:

    int                    fEventSet = 0; // PAPI event set
    papi_counter_t         fStartedAt[N] = {0};
    papi_counter_t         fIntervals[N] = {0};
    int                    fEvent[N] = {0};
};

template <unsigned N>
PapiCounter<N>::PapiCounter(std::initializer_list<int> events) {

    fEventSet = PAPI_NULL;

    if (LLVM_UNLIKELY(codegen::PapiCounterOptions.compare(codegen::OmittedOption) == 0)) {

        // PAPI init
        int rval = PAPI_library_init(PAPI_VER_CURRENT);
        if (rval != PAPI_VER_CURRENT) {
            throw std::runtime_error("PAPI Library Init Error: " + std::string(PAPI_strerror(rval)));
        }
        std::copy(events.begin(), events.end(), fEvent);

        // PAPI create event set
        if ((rval = PAPI_create_eventset(&fEventSet)) != PAPI_OK) {
            throw std::runtime_error("PAPI Create Event Set Error: " + std::string(PAPI_strerror(rval)));
        }
        assert (fEventSet != PAPI_NULL);
        // PAPI fill event set
        if ((rval = PAPI_add_events(fEventSet, fEvent, N)) != PAPI_OK) {
            throw std::runtime_error("PAPI Add Events Error: " + std::string(PAPI_strerror(rval < PAPI_OK ? rval : PAPI_EINVAL)));
        }

        // Call PAPI start on construction, to force PAPI initialization
        rval = PAPI_start(fEventSet);
        if (rval != PAPI_OK) {
            throw std::runtime_error("PAPI Start Error: " + std::string(PAPI_strerror(rval)));
        }

    }
}

template <unsigned N>
PapiCounter<N>::~PapiCounter() {
    if (LLVM_LIKELY(fEventSet != PAPI_NULL)) {
        auto rval =  PAPI_cleanup_eventset(fEventSet);
        if (rval != PAPI_OK) {
            throw std::runtime_error("PAPI code: " + std::string(PAPI_strerror(rval)));
        }
        // PAPI free all events
        if ((rval = PAPI_destroy_eventset(&fEventSet) != PAPI_OK)) {
            throw std::runtime_error("PAPI code: " + std::string(PAPI_strerror(rval)));
        }
        PAPI_shutdown();
    }
}

template <unsigned N>
void PapiCounter<N>::start() { 
    if (LLVM_LIKELY(fEventSet != PAPI_NULL)) {
        const auto rval = PAPI_read(fEventSet, fStartedAt);
        if (rval != PAPI_OK) {
            throw std::runtime_error("PAPI code: " + std::string(PAPI_strerror(rval)));
        }
    }
}

// PAPI Low Level API Wrapper: Records the difference Events of the current start interval array values set into the values.
template <unsigned N>
void PapiCounter<N>::stop() {
    if (LLVM_LIKELY(fEventSet != PAPI_NULL)) {
        papi_counter_t endedAt[N];
        const auto rval = PAPI_stop(fEventSet, endedAt);
        if (rval != PAPI_OK) {
            throw std::runtime_error("PAPI code: " + std::string(PAPI_strerror(rval)));
        }
        for (unsigned i = 0; i != N; i++) {
            fIntervals[i] = (endedAt[i] - fStartedAt[i]);
        }
    }
}

template <unsigned N>
void PapiCounter<N>::write(std::ostream & out) const {
    if (LLVM_LIKELY(fEventSet != PAPI_NULL)) {
        // Convert PAPI codes to names
        char eventName[PAPI_MAX_STR_LEN + 1];
        for (unsigned i = 0; i != N; i++) {
            int rval = PAPI_event_code_to_name(fEvent[i], eventName);
            if (rval != PAPI_OK) {
                memset(eventName + 1, '?', 16);
                eventName[16 + 1] = 0;
            }
            out << ';' << eventName << '|' << fIntervals[i];
        }
    }
}

inline static std::ostream & operator << (std::ostream & out, const IPapiCounter & papiCounter) {
    papiCounter.write(out);
    return out;
}

}

#endif // PAPICOUNTER_HPP
