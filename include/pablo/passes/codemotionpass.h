#pragma once

namespace pablo {
class PabloKernel;
class CodeMotionPass {
public:
    static bool optimize(PabloKernel * const kernel);
};
}

