@echo off
echo Setting up third party dependencies...

mkdir source\lib 2>nul
curl -o source\lib\cJSON.h https://raw.githubusercontent.com/DaveGamble/cJSON/master/cJSON.h
curl -o source\lib\cJSON.c https://raw.githubusercontent.com/DaveGamble/cJSON/master/cJSON.c

echo Setup complete! cJSON downloaded successfully.
pause
