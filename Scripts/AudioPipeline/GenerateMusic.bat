@echo off
rem Usage: GenerateMusic.bat "style tags" out.wav [--seconds 60 --seed 0 --count 3 --lyrics "[instrumental]"]
"D:\AudioGen\Venv\Scripts\python.exe" -u "%~dp0GenerateMusic.py" %*
