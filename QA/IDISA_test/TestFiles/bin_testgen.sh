#!/bin/bash
dd if=/dev/urandom of=randvec1.bin bs=1K count=64
dd if=/dev/urandom of=randvec2.bin bs=1K count=64
dd if=/dev/urandom of=randvec3.bin bs=1K count=64