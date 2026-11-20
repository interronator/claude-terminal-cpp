@echo off
rem Usage: GenerateVoice.bat "text"|lines.csv outDir [--engine chatterbox|kokoro --voice ref.wav --emotion 0.5]
"D:\AudioGen\Venv\Scripts\python.exe" -u "%~dp0GenerateVoice.py" %*
