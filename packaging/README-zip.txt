Zodiac kit (portable zip)
=========================

Zodiac is a game development kit for blind developers. This zip is the whole kit: unzip the Zodiac folder anywhere you like. It works from any folder, including folders with spaces or non-English letters in their names, and it needs nothing else installed.

What is inside
--------------
bin            zodiac.exe (the command line tool), zodiacw.exe (the same tool without a console window, for the right-click entry) and the linker with its libraries.
lib            The libraries zodiac build links your game with.
modules        The modules your games can use.
include        The header files for writing your own modules.
docs           The manual.
examples       A small example project, examples\hello.
LICENSE, THIRD_PARTY_LICENSES   The license of Zodiac and of the software it contains.

Memory and Windows versions
---------------------------
Zodiac and every game it builds ask Windows for its Segment Heap, a memory allocator that stays fast in large games. Windows 10 version 2004 and later use it. Older Windows ignores the request and uses its classic heap, so games run there exactly as before.

Adding Zodiac to your PATH
--------------------------
To run zodiac from any folder, add the bin folder to your PATH.
1. Press the Windows key, type "environment variables" and open "Edit environment variables for your account".
2. Select the Path variable in the user variables list and choose Edit.
3. Choose New, type the full path of the bin folder, for example C:\Tools\Zodiac\bin, and press Enter.
4. Confirm every dialog with OK, then open a new terminal window.
Or, for the current terminal window only:
   set PATH=C:\Tools\Zodiac\bin;%PATH%

Making your first game
----------------------
1. Create a project:
   zodiac new mygame
2. Write your game in mygame\main.zs.
3. Run it while you work on it:
   zodiac run mygame
4. Build the finished game; the result is mygame\release\mygame.exe:
   zodiac build mygame

Run "zodiac help" to see every command. If you did not add bin to your PATH, call zodiac.exe by its full path, for example C:\Tools\Zodiac\bin\zodiac.exe new mygame.
