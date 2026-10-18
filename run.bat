@echo off
rem Starts the built program (its Qt and MuPDF libraries are in MSYS2's UCRT64 folder).
set PATH=C:\msys64\ucrt64\bin;%PATH%
start "" "%~dp0build\MasterPDF.exe" %*
