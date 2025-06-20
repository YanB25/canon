#!/usr/bin/python3
import util
import sys
targets = sys.argv[1:]
util.build_release(targets, silent=False)
