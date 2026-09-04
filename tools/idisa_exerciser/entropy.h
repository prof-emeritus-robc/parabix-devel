#pragma once

#include <stdint.h>
#include <stdlib.h>
#if defined(__APPLE__)
# include <sys/random.h>
#else
# include <unistd.h>
#endif

class Entropy {
  public:
    uint64_t nextWord() { return next<uint64_t>(); }

    template <class T> T next() {
        T result;
        fill(&result, sizeof result);
        return result;
    }

    static void fill(void *buf, size_t size) {
        if (getentropy(buf, size) != 0) {
            abort();
        }
    }

    template <class T> static void fill(T &buf) {
        fill((void *)&*begin(buf), (uint8_t *)&*end(buf) - (uint8_t *)&*begin(buf));
    }
};
