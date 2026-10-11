#!/usr/bin/env python3
"""python3 tools/desktop_session.py <list|setup|start|env|tool|shot|stop|status> ..., see tools/desktops/session.py"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from desktops.__main__ import main  # noqa: E402

main()
