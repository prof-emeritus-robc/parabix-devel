#pragma once

namespace pablo {

class PabloKernel;

class DistributivePass {
public:
    static bool optimize(pablo::PabloKernel * const kernel);
};

}

