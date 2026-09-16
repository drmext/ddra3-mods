@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
cd /d "%~dp0"

if not exist obj64 mkdir obj64

ml64 /nologo /c /Foobj64\life_add_cave.obj life_add_cave.asm || exit /b 1

cl /nologo /O2 /W3 /MT /EHs- /GS- /I ..\minhook\include ^
  /Foobj64\ /Fdobj64\retry_exit_64bit.pdb ^
  retry_exit.cpp ^
  ..\minhook\src\buffer.c ..\minhook\src\hook.c ^
  ..\minhook\src\trampoline.c ..\minhook\src\hde\hde64.c ^
  /link /DLL /MACHINE:X64 /OUT:retry_exit_64bit.dll /PDB:retry_exit_64bit.pdb ^
  obj64\life_add_cave.obj user32.lib /OPT:REF /OPT:ICF /INCREMENTAL:NO || exit /b 1

echo Built retry_exit_64bit.dll
endlocal
