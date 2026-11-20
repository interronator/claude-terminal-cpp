@echo off
rem One-time setup of D:\AudioGen (needs uv with Python 3.11).
set HF_HOME=D:\AudioGen\HfCache
set PY=D:\AudioGen\Venv\Scripts\python.exe
if exist %PY% (echo venv exists) else (uv venv D:\AudioGen\Venv --python 3.11 || exit /b 1)
uv pip install --python %PY% torch==2.6.0 torchaudio==2.6.0 --index-url https://download.pytorch.org/whl/cu126 || exit /b 1
uv pip install --python %PY% soundfile numpy setuptools wheel || exit /b 1
uv pip install --python %PY% --no-build-isolation-package pkuseg chatterbox-tts kokoro git+https://github.com/hkchengrex/MMAudio.git || exit /b 1
uv pip install --python %PY% git+https://github.com/ace-step/ACE-Step.git || exit /b 1
uv pip install --python %PY% --reinstall-package torch --reinstall-package torchaudio torch==2.6.0 torchaudio==2.6.0 --index-url https://download.pytorch.org/whl/cu126 || exit /b 1
rem chatterbox-tts pins torch 2.6.0 from PyPI (CPU only on Windows); the last line puts the CUDA build back
