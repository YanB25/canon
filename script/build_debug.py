#!/usr/bin/python3
import util
import sys
targets = sys.argv[1:]
util.build_debug(targets, silent=False)
