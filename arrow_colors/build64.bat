@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
cd /d "%~dp0"

if not exist obj64 mkdir obj64

ml64 /nologo /c /Foobj64\arrow_colors_cave.obj arrow_colors_cave.asm || exit /b 1

cl /nologo /O2 /W3 /MT /EHs- /GS- /I ..\minhook\include ^
  /Foobj64\ /Fdobj64\arrow_colors_64bit.pdb ^
  arrow_colors.cpp ^
  ..\minhook\src\buffer.c ..\minhook\src\hook.c ^
  ..\minhook\src\trampoline.c ..\minhook\src\hde\hde64.c ^
  /link /DLL /MACHINE:X64 /OUT:arrow_colors_64bit.dll /PDB:arrow_colors_64bit.pdb ^
  obj64\arrow_colors_cave.obj user32.lib /OPT:REF /OPT:ICF /INCREMENTAL:NO || exit /b 1

echo Built arrow_colors_64bit.dll
endlocal
