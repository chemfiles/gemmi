#!/bin/sh

set -eu
cd "$(dirname "$0")"
(cd src && make -j4 all write-help)
(cd python && make -j4)
(cd docs && make -j4 html SPHINXOPTS="-q -n")
PYTHONPATH=python python -m unittest discover -s tests
# Where python 2 and 3 output differ, the docs have output from v3.
# So 'make doctest' works only if sphinx-build was installed for python3.
(cd docs && make doctest SPHINXOPTS="-q -n")
flake8
