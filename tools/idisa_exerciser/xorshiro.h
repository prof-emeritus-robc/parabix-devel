#pragma once

// Adapted from version on Wikipedia, August 2026
// That version is credited as adapted from Sebastiano Vigna

#include <stdint.h>

#include "entropy.h"

class XorShift256pp {
  public:
    explicit XorShift256pp(uint64_t seed) { init(seed); }

    void init(uint64_t seed) {
        uint64_t ms = seed;
        // Use mix64 for init: at least part of the state will be nonzero (as required to prevent xorshiro from getting
        // stuck), no matter what seed goes in
        auto mix64 = [&]() {
            ms += 0x9E3779B97F4A7C15;
            uint64_t result = ms;
            result = (result ^ (result >> 30)) * 0xBF58476D1CE4E5B9;
            result = (result ^ (result >> 27)) * 0x94D049BB133111EB;
            return result ^ (result >> 31);
        };

        for (auto &si : s) {
            si = mix64();
        }
    };

    uint64_t nextWord() {
        auto rol64 = [](uint64_t x, int k) { return (x << k) | (x >> (64 - k)); };

        const uint64_t result = rol64(s[1] * 5, 7) * 9;
        const uint64_t t = s[1] << 17;

        s[2] ^= s[0];
        s[3] ^= s[1];
        s[1] ^= s[2];
        s[0] ^= s[3];

        s[2] ^= t;
        s[3] = rol64(s[3], 45);

        return result;
    }

    // template <class T> T next() {}

    static uint64_t entropySeed() { return Entropy().nextWord(); }

  private:
    uint64_t s[4];
};
