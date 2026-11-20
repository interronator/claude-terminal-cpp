@echo off
rem Usage: GenerateSfx.bat "prompt" out.wav [--seconds 4 --seed 0 --count 4 --video clip.mp4]
"D:\AudioGen\Venv\Scripts\python.exe" -u "%~dp0GenerateSfx.py" %*
