@echo off
echo Starting Camera Capture Service...
cd /d "%~dp0camera-capture"
python -u main.py
pause
