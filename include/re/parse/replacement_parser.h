/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <string>

namespace re {

class RE;

//
// The replacement pattern of a search and replace, as an RE: a sequence of
// literal characters (CCs of one Unicode codepoint each) and references to
// the captures of the search pattern (re::Reference).
//
// Syntax (UTF-8):
//   $n       the capture numbered n (one or more decimal digits)
//   ${n}     the capture numbered n
//   ${name}  the capture of that name (letters, digits and underscores)
//   \n       the capture numbered n, for one digit n from 1 to 9
//   $$ \$    a literal $
//   \\       a literal backslash
//   \t \n \r \f \v \e \a
//            tab, newline, carriage return, form feed, vertical tab, escape,
//            alarm (\n is a newline: \ followed by the letter n)
//   \xHH \x{H...}
//            the codepoint given in hexadecimal
//   any other character, literally.
// The capture numbered n is the capture named "\n" (as the RE parser names
// them) or, failing that, the capture named "n" (as LDML rules name them).
//
// Returns nullptr, with a message in error, if the replacement is malformed
// or refers to a capture that the search pattern does not have.
//
RE * parseReplacement(const std::string & replacement, RE * search, std::string & error);

}
