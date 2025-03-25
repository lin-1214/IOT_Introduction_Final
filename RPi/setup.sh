#!/bin/bash

sudo apt-get update
sudo apt-get install -y python3-pip

# Try normal pip install first, if it fails, try with --break-system-packages
pip3 install -r requirements.txt || pip3 install -r requirements.txt --break-system-packages