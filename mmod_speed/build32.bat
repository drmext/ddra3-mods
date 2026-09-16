@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars32.bat" || exit /b 1
cd /d "%~dp0"

if not exist obj32 mkdir obj32

ml /nologo /c /coff /Foobj32\option_copy_cave32.obj option_copy_cave32.asm || exit /b 1

cl /nologo /O2 /W3 /MT /EHs- /GS- /I ..\minhook\include ^
  /Foobj32\ /Fdobj32\mmod_speed_32bit.pdb ^
  mmod_speed.cpp ^
  ..\minhook\src\buffer.c ..\minhook\src\hook.c ^
  ..\minhook\src\trampoline.c ..\minhook\src\hde\hde32.c ^
  /link /DLL /MACHINE:X86 /OUT:mmod_speed_32bit.dll /PDB:mmod_speed_32bit.pdb ^
  obj32\option_copy_cave32.obj user32.lib /OPT:REF /OPT:ICF /INCREMENTAL:NO || exit /b 1

echo Built mmod_speed_32bit.dll
endlocal
