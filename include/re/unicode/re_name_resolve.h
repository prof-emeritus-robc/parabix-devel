#pragma once


namespace re {

    class RE;
    class Name;
    enum class NameStandard {Posix, Unicode};
    RE * resolveEscapeNames(RE * re, NameStandard c = NameStandard::Unicode);
}
