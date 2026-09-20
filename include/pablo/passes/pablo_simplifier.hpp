#pragma once

namespace pablo {

class PabloKernel;

class Simplifier {
public:
    static bool optimize(PabloKernel * kernel);
protected:
    Simplifier() = default;
};

}
