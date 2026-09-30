#pragma once

#include <vector>

namespace re {

class CC;
class RE;

//  Given a fixed-length sequence of character classes, count the positions
//  within strings matched by a given RE at which a string formed by
//  concatenating one character each from these classes could end.
//
//  The count is an upper bound over all strings matched by the RE: a result
//  of 0 means that no such substring can occur, and a result of k means that
//  at most k occurrences can be found in any matched string.  The result is
//  -1 if the RE contains unsupported constructs (e.g., back references), or
//  if the number of occurrences is unbounded.
//

int CC_Sequence_Search(std::vector<CC *> & CC_seq, RE * re);

}
