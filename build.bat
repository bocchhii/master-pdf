@echo off
rem Builds Master PDF (C++) with MSYS2's UCRT64 tools: Qt 6, MuPDF, CMake, Ninja.
rem The program ends up in build\MasterPDF.exe - start it with run.bat.
set PATH=C:\msys64\ucrt64\bin;%PATH%
cmake -S "%~dp0." -B "%~dp0build" -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build "%~dp0build" || exit /b 1
echo.
echo Built: %~dp0build\MasterPDF.exe
