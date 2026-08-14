#!/bin/bash
echo "Setting up third party dependencies..."

mkdir -p source/lib
curl -o source/lib/cJSON.h https://raw.githubusercontent.com/DaveGamble/cJSON/master/cJSON.h
curl -o source/lib/cJSON.c https://raw.githubusercontent.com/DaveGamble/cJSON/master/cJSON.c

echo "Setup complete! cJSON downloaded successfully."
