@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars32.bat" || exit /b 1
cd /d "%~dp0"

if not exist obj32 mkdir obj32

cl /nologo /O2 /W3 /MT /EHs- /GS- /I ..\minhook\include ^
  /Foobj32\ /Fdobj32\pfree_ghost_32bit.pdb ^
  pfree_ghost.cpp ^
  ..\minhook\src\buffer.c ..\minhook\src\hook.c ^
  ..\minhook\src\trampoline.c ..\minhook\src\hde\hde32.c ^
  /link /DLL /MACHINE:X86 /OUT:pfree_ghost_32bit.dll /PDB:pfree_ghost_32bit.pdb ^
  user32.lib /OPT:REF /OPT:ICF /INCREMENTAL:NO || exit /b 1

echo Built pfree_ghost_32bit.dll
endlocal
