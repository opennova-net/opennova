"""Legacy setuptools entry point.

PEP 660 editable installs require pip 21.3+; 3ds Max 2022 ships pip
20.1.1 which only knows the legacy ``setup.py develop`` flow. This
three-line shim delegates to setuptools, which reads metadata from
``pyproject.toml`` as usual. Drop this file once Max's bundled pip is
new enough to be reliable across the supported version range.
"""
from setuptools import setup

setup()
