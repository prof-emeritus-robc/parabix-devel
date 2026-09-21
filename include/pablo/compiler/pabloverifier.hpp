#pragma once

#include <string>

namespace pablo {

class PabloKernel;

class PabloVerifier {
public:
    static void verify(const PabloKernel * kernel, const std::string & location = "");
};

}

