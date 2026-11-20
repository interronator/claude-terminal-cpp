@echo off
rem Usage: CleanupMesh.bat --input in.glb --name Crate --kind prop --size 1.2 [--tris N --tex 2048 --rotate-z 0]
"D:\Blender\blender.exe" -b -P "%~dp0CleanupMesh.py" -- %*
