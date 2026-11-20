@echo off
rem Usage: CleanupAudio.bat in.wav out.wav [--kind sfx|voice|music --loop-seconds 1.5 --fade-ms 8]
"D:\AudioGen\Venv\Scripts\python.exe" -u "%~dp0CleanupAudio.py" %*
