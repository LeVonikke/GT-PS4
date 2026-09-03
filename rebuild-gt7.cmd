@echo off
cd /d "F:\Ferramentas\AI Sandbox\Projetos Codex\GT7HACKFIX\shadPS4"
call "F:\Ferramentas\DevTools\VisualStudio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
"F:\Ferramentas\DevTools\CMake\cmake-3.31.5-windows-x86_64\bin\cmake.exe" --build build\x64-Release --target shadps4 -- -j4
