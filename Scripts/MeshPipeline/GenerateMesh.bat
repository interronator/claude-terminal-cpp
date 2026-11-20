@echo off
rem Usage: GenerateMesh.bat image.png out.glb [--seed N --pipeline 1024_cascade --texture 4096 --faces 1000000]
"D:\MeshGen\Venv\Scripts\python.exe" -u "%~dp0GenerateMesh.py" %*
