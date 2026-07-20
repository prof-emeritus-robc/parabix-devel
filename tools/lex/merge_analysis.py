#  merge_analysis functions
#
def load_merges_raw(file_path):
	merges = []
	with open(file_path, "r", encoding="utf-8") as f:
		base_id = 256
		for line in f:
			line = line.strip()
			# Skip comments or version headers (e.g., #version: 0.2)
			if not line or line.startswith("#version"):
			    continue
			# Split the paired tokens by whitespace
			token_pair = line.split()
			merges.append([base_id, token_pair[0], token_pair[1]])
			base_id += 1
		return merges

def base_byte_id_map():
	vocab_ = {}
	idToToken_ = {}
	i = 0
	for printable in range(33, 127):
		ch = chr(printable)
		vocab_[ch] = i
		idToToken_[i] = ch
		i += 1 
	for printable in range(161, 173):
		ch = chr(printable)
		vocab_[ch] = i
		idToToken_[i] = ch
		i += 1 
	for printable in range(174, 256):
		ch = chr(printable)
		vocab_[ch] = i
		idToToken_[i] = ch
		i += 1
	cp = 256
	for control in range(0, 33):
		ch = chr(cp)
		vocab_[ch] = i
		idToToken_[i] = ch
		i += 1
		cp += 1
	for control in range(127, 161):
		ch = chr(cp)
		vocab_[ch] = i
		idToToken_[i] = ch
		i += 1
		cp += 1
	for control in range(173, 174):
		ch = chr(cp)
		vocab_[ch] = i
		idToToken_[i] = ch
		i += 1
		cp += 1
	return (vocab_, idToToken_)

def make_merge_maps(merges):
	(vocab_, idToToken_) = base_byte_id_map()
	for [base_id, t1, t2] in merges:
		merge = t1 + t2
		vocab_[merge] = base_id
		idToToken_[base_id] = [t1, t2]
	return (vocab_, idToToken_)

def range_for_bits(b):
	hi = 1<<b - 1
	lo = 1 <<(b-1)
	return (lo, hi)

def bits(n):
	return n.bit_length()

#
#  For all merges in a particular range, determine which
#  ones depend on a prior merge within the range itself.
def subrange_dependency_analysis(maps, lo, hi):
	(vocab_, idToToken_) = maps
	id0_in_range = 0
	id1_in_range = 0
	for vocab_id in range(lo, hi):
		merge = idToToken_[vocab_id]
		id0 = vocab_[merge[0]]
		id1 = vocab_[merge[1]]
		if id0 >= lo: 
			id0_in_range += 1
		if id1 >= lo:
			id1_in_range += 1
		if id0 >= lo or id1 >= lo:
			print("%i = %s(%i) %s(%i)" %(vocab_id, merge[0], id0, merge[1], id1))
	return (id0_in_range, id1_in_range)

#
#  Sort all merges in a particular range and group them
#  by their first merge.

def subrange_left_factor_analysis(maps, lo, hi):
	(vocab_, idToToken_) = maps
	left_factor_group = {}
	left_factor_count = {}
	for vocab_id in range(lo, hi):
		merge = idToToken_[vocab_id]
		id0 = vocab_[merge[0]]
		id1 = vocab_[merge[1]]
		if not id0 in left_factor_group.keys():
			left_factor_group[id0] = "$ = %s(%i) =>" %(merge[0], id0)
			left_factor_count[id0] = 0
		left_factor_group[id0] += " %i = $ %s(%i);" %(vocab_id, merge[1], id1)
		left_factor_count[id0] += 1
	for lfid in sorted(left_factor_group.keys()):
		if left_factor_count[lfid] > 1:
			print(left_factor_group[lfid])

def idToStr(maps, id_):
	(vocab_, idToToken_) = maps
	tok = idToToken_[id_]
	if id_ < 256:
		return tok
	else:
		return tok[0] + " " + tok[1]

#  Two merges OVERLAP by TOKEN-ID (token-adjacency): the RIGHT part token of one
#  equals the LEFT part token of the other, so on the id stream they fight over
#  that shared middle token's start. Pure id equality; a = (leftId, rightId).
#      right(A) == left(B)   OR   right(B) == left(A)
#  Matches the start-anchored kernel's token-grid competition (not raw bytes).
#  Same-left / same-right factors never match here (only the two CROSS pairs are
#  compared), so left-factoring / right-factoring cases are excluded by design.
def merges_overlap(a, b):
	return a[1] == b[0] or b[1] == a[0]

#
#  find all pairs of merges in a range that OVERLAP — i.e. could both trigger over
#  a shared middle token (merges_overlap on their part ids: right of one == left
#  of the other).
#
def subrange_conflict_analysis(maps, lo, hi):
	(vocab_, idToToken_) = maps
	for idA in range(lo, hi - 1):
		mergeA = idToToken_[idA]
		idA_0 = vocab_[mergeA[0]]; idA_1 = vocab_[mergeA[1]]
		for idB in range(idA + 1, hi):
			mergeB = idToToken_[idB]
			idB_0 = vocab_[mergeB[0]]; idB_1 = vocab_[mergeB[1]]
			# skip if common left factors (left-factoring handles equal left tokens)
			if idA_0 == idB_0: continue
			if not merges_overlap((idA_0, idA_1), (idB_0, idB_1)): continue
			# conflict
			print("%i = %s  X  %i = %s" % (idA, idToStr(maps, idA), idB, idToStr(maps, idB)))

#
#  Given a range of merges, 
#  (1) identify conflicts between any pair of merge ids idA and idB such that
#      (a) idA < idB and
#      (b) leftID[idA] = rightID[idB]
#  (2) resolve the conflict by adding a negative assertion for idB for the rightId[idA]

def subrange_conflict_resolution(maps, lo, hi):
	(vocab_, idToToken_) = maps
	byLeftID = {}
	for idA in range(lo, hi - 1):
		mergeA = idToToken_[idA]
		idA_0 = vocab_[mergeA[0]]; idA_1 = vocab_[mergeA[1]]
		if idA_1 in byLeftID.keys():
			excluded = ", ".join([idToStr(maps, x) for x in byLeftID[idA_1]])
			print("%i = %s (?! %s )" % (idA, idToStr(maps, idA), excluded))
		if not idA_0 in byLeftID.keys():
			byLeftID[idA_0] = [idA_1]
		else:
			byLeftID[idA_0].append(idA_1)


#
#  Generate the expansion of a token_id into a merge of
#  tokens whose IDs are all less than the given lo value.
def expand_token(maps, token_id, lo):
	(vocab_, idToToken_) = maps
	tok = idToToken_[token_id]
	if token_id < 256:
		return "%s (%i)" % (tok, token_id)
	if token_id < lo:
		return "%s%s (%i)" % (tok[0], tok[1], token_id)
	else:
		id0 = vocab_[tok[0]]
		id1 = vocab_[tok[1]]
		return "%s %s"% (expand_token(maps, id0, lo), expand_token(maps, id1, lo))

#
#  For all merges in a particular range such that the right token id
#  is also in the range:  generate an expanded merge such that all but
#  the first one token is guaranteed to be lower than the lo value.
#
def subrange_right_factor_analysis(maps, lo, hi):
	(vocab_, idToToken_) = maps
	id0_in_range = 0
	id1_in_range = 0
	for vocab_id in range(lo, hi):
		merge = idToToken_[vocab_id]
		id0 = vocab_[merge[0]]
		id1 = vocab_[merge[1]]
		if id1 > lo:
			print("%i = %s(%i) %s" %(vocab_id, merge[0], id0, expand_token(maps, id1, lo)))

#
#  Find the largest contiguous range of merges starting at lo
#  such that none of the merges is dependent on any id >= lo.
#  
def independent_range_limit(maps, lo):
	(vocab_, idToToken_) = maps
	vocab_id = lo
	if not vocab_id in idToToken_.keys(): return lo
	merge = idToToken_[vocab_id]
	id0 = vocab_[merge[0]]
	id1 = vocab_[merge[1]]
	while id0 < lo and id1 < lo:
		vocab_id += 1
		if not vocab_id in idToToken_.keys(): return vocab_id
		merge = idToToken_[vocab_id]
		id0 = vocab_[merge[0]]
		id1 = vocab_[merge[1]]
	return vocab_id

#  Partition the merge data into independent ranges such that
#  in any such range, no merge depends on any id greater that
#  is produced within the range (or a higher range).
def independent_range_analysis(maps):
	base_id = 256
	ranges = []
	cur_id = base_id
	limit = independent_range_limit(maps, cur_id)
	while limit != cur_id:
		ranges.append((cur_id, limit))
		cur_id = limit
		limit = independent_range_limit(maps, cur_id)
	return ranges

#
#  Largest contiguous range starting at lo that is BOTH
#    (1) dependency-independent — no merge uses an id >= lo (as before), AND
#    (2) overlap-free by TOKEN-ID — no two merges compete for a shared middle
#        token: merges_overlap on their part ids (right of one == left of other).
#        Matches the start-anchored kernel's token-grid competition.
#  Stop at the first merge that violates either.
def clean_range_limit(maps, lo):
	(vocab_, idToToken_) = maps
	accepted = []                             # (leftId, rightId) part-id pairs in this range
	vocab_id = lo
	while vocab_id in idToToken_.keys():
		merge = idToToken_[vocab_id]
		id0 = vocab_[merge[0]]
		id1 = vocab_[merge[1]]
		if id0 >= lo or id1 >= lo:            # dependency violation
			break
		cur = (id0, id1)
		if any(merges_overlap(cur, p) for p in accepted):   # token-adjacency overlap
			break
		accepted.append(cur)
		vocab_id += 1
	# the first merge at lo always fits (its parts precede it and set is empty),
	# so this never stalls
	return vocab_id

#  Partition the merge data into ranges that are independent AND conflict-free.
def clean_range_analysis(maps):
	base_id = 256
	ranges = []
	cur_id = base_id
	limit = clean_range_limit(maps, cur_id)
	while limit != cur_id:
		ranges.append((cur_id, limit))
		cur_id = limit
		limit = clean_range_limit(maps, cur_id)
	return ranges

#  Which range (index) does a given merge id fall in?
def range_index_of(ranges, vocab_id):
	for i, (lo, hi) in enumerate(ranges):
		if lo <= vocab_id < hi:
			return i
	return None

#  Which range does a token STRING fall in? Looks up the id, then the range.
def range_for_token(maps, ranges, tok):
	(vocab_, idToToken_) = maps
	if tok not in vocab_:
		print("token %r not in vocab" % tok)
		return None
	vid = vocab_[tok]
	for (lo, hi) in ranges:
		if lo <= vid < hi:
			print("%r  id=%d  range=(%d, %d)  index=%d" % (tok, vid, lo, hi, ranges.index((lo, hi))))
			return (lo, hi)
	print("%r  id=%d  not in any range" % (tok, vid))
	return None
