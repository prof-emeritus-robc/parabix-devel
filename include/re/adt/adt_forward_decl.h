/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

// This file provides a forward delcation for all RE ADT types.
// Include re/adt/adt.h for full type definitions.

#pragma once

namespace re {

class RE;

class Name; class Start; class End;   class CC; class Any;
class Seq;  class Alt;   class Rep;   class Intersect; 
class Diff; class Range; class Group; class Assertion;
class Capture; class Reference; class PropertyExpression;
class Permute; class Interleavable;

}
