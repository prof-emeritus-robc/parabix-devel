#pragma once

namespace pablo {

class PabloKernel;

class SchedulingPrePass {
public:
    static bool optimize(PabloKernel * kernel);
};

}

