@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
cd /d "%~dp0"

if not exist obj mkdir obj

cl /nologo /O2 /W3 /MT /EHs- /GS- /I ..\minhook\include ^
  /Foobj\ /Fdobj\bpl_loop.pdb ^
  bpl_loop.cpp ^
  ..\minhook\src\buffer.c ..\minhook\src\hook.c ..\minhook\src\trampoline.c ..\minhook\src\hde\hde64.c ^
  /link /DLL /MACHINE:X64 /OUT:bpl_loop.dll /PDB:bpl_loop.pdb ^
  user32.lib /OPT:REF /OPT:ICF /INCREMENTAL:NO || exit /b 1

echo Built bpl_loop.dll
endlocal
