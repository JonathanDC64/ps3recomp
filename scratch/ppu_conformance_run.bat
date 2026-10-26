@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
cd /d "C:\Users\jonat\AppData\Local\Temp\claude\c--dev-projects-demons-souls-pc-port\5557b0e9-6263-44c5-8be5-a377c30e0eb5\scratchpad\w\ps3r-try"
cl /nologo /O1 /W3 /I runtime\ppu /Fe:"C:\Users\jonat\AppData\Local\Temp\claude\c--dev-projects-demons-souls-pc-port\5557b0e9-6263-44c5-8be5-a377c30e0eb5\scratchpad\w\ps3r-try\scratch\ppu_conformance.exe" "C:\Users\jonat\AppData\Local\Temp\claude\c--dev-projects-demons-souls-pc-port\5557b0e9-6263-44c5-8be5-a377c30e0eb5\scratchpad\w\ps3r-try\scratch\ppu_conformance.cpp" > "C:\Users\jonat\AppData\Local\Temp\claude\c--dev-projects-demons-souls-pc-port\5557b0e9-6263-44c5-8be5-a377c30e0eb5\scratchpad\w\ps3r-try\scratch\ppu_conformance.log" 2>&1
if errorlevel 1 exit /b 2
"C:\Users\jonat\AppData\Local\Temp\claude\c--dev-projects-demons-souls-pc-port\5557b0e9-6263-44c5-8be5-a377c30e0eb5\scratchpad\w\ps3r-try\scratch\ppu_conformance.exe" >> "C:\Users\jonat\AppData\Local\Temp\claude\c--dev-projects-demons-souls-pc-port\5557b0e9-6263-44c5-8be5-a377c30e0eb5\scratchpad\w\ps3r-try\scratch\ppu_conformance.log" 2>&1
