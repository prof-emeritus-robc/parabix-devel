#
# abc_testgen.py
#
# Generate grep test cases for ab*c pattern to segment
# handling support (long lines, b* crossing segment boundaries).
#
# Also writes abc/QuickTests.txt, the cases that abc_test.py runs by default:
# QUICK_CASE_COUNT cases evenly spaced by match length, from the shortest match
# to the longest.  abc_test.py --extended-testing runs all the cases.
# python3 abc_testgen.py --quick-list-only writes the list for the existing
# test files without generating new ones.
#
from random import randint
import os
import re
import shutil
import sys
#
#
output_home = 'abc'
QUICK_CASE_COUNT = 8

#
# Generate a line that matches ab*c with a given offset from 
# the line beginning, a given length of the match and a given
# total line length (including new line).
#
def generate_line_to_match(offset, match_length, line_length):
  return "%sa%sc%s\n" % (('_' * offset), ("b" * (match_length - 2)), ('_' * (line_length - (offset + match_length) - 1)))

#
# Generate lines that do not match ab*c of total length lgth.
#
def generate_non_match_lines(lgth):
  lgth_remain = lgth
  linedata = ""
  while lgth_remain > 0:
    this_line_lgth = randint(1, lgth_remain)
    if this_line_lgth == 1: linedata += "\n"
    else: linedata += "a%s\n" % ("b" * (this_line_lgth - 2))
    lgth_remain -= this_line_lgth
  return linedata

def generate_case(line_pos, match_pos, match_length, line_length, extra):
  file_name = "ab{%i}c@%i.txt" % (match_length - 2, line_pos+match_pos)
  line_to_match = generate_line_to_match(match_pos-line_pos, match_length, line_length)
  f1 = open(output_home + "/ExpectedOutput/" + file_name, "w")
  f1.write(line_to_match)
  f1.close()
  f2 = open(output_home + "/TestFiles/" + file_name, "w")
  f2.write(generate_non_match_lines(line_pos))
  f2.write(line_to_match)
  f2.write(generate_non_match_lines(extra))
  f2.close()

#
# Write the names of QUICK_CASE_COUNT test files, evenly spaced by match length,
# to QuickTests.txt.
#
def write_quick_list():
  def match_length(file_name):
    return int(re.match(r"ab\{(\d+)\}c@", file_name).group(1))
  names = sorted(os.listdir(output_home + "/TestFiles"), key=lambda n: (match_length(n), n))
  count = min(QUICK_CASE_COUNT, len(names))
  if count <= 1: quick = names[:count]
  else: quick = [names[(i * (len(names) - 1)) // (count - 1)] for i in range(count)]
  with open(output_home + "/QuickTests.txt", "w") as f:
    for name in quick:
      f.write(name + "\n")

def main():
  if "--quick-list-only" in sys.argv[1:]:
    write_quick_list()
    return
  if os.path.exists(output_home + ".bak" ):
    shutil.rmtree(output_home + ".bak")
  if os.path.exists( output_home ):
    shutil.move(output_home, output_home + ".bak")
  os.mkdir(output_home)
  os.mkdir(output_home + "/TestFiles/")
  os.mkdir(output_home + "/ExpectedOutput/")

  for block_size in [64, 128, 1024]:
     for segment_blocks in [2,3,5,11,15,19,25,30,31,32,43]:
        segment_base = block_size * segment_blocks
        for segments_in_match in range(4):
          additional_blocks = randint(0, segment_blocks - 1)
          match_blocks = segments_in_match * segment_blocks + additional_blocks
          match_extra = randint(0, block_size - 1)
          match_length = match_blocks * block_size + match_extra
          min_segment_offset = segment_base - (additional_blocks * block_size + match_extra)   
          max_segment_offset = segment_base - match_extra
          match_abs_pos = randint(min_segment_offset, max_segment_offset) + randint(0, 10) * segment_base
          line_abs_pos = match_abs_pos - randint(1, match_abs_pos)
          line_length = (match_abs_pos - line_abs_pos) + match_length + randint(1, segment_base)
          extra = randint(0, segment_base * 4)
          generate_case(line_abs_pos, match_abs_pos, match_length, line_length, extra)
  write_quick_list()

if __name__ == "__main__":
  main()


 
