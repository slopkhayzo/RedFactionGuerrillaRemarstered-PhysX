rfgphys @VERSION@ - physics mod for Red Faction Guerrilla Re-Mars-tered
=====================================================================

What it does
------------
- Debris is never removed: buildings you bring down stay as rubble.
- Debris pieces are simulated by NVIDIA PhysX 5 on the graphics card, in a separate process, so piles of tens of
  thousands of pieces keep a high frame rate.
- Debris pieces collide with each other and pile up, explosions throw piles around, and you can stand on heavy
  rubble while lighter pieces get pushed aside.
- The game's own physics (Havok) runs on several CPU cores.
- Tweaks for heavy scenes: no GPU occlusion queries, and small far-away objects cast no shadows.

Works in the campaign and in Wrecking Crew. Its changes are never applied in multiplayer.

Requirements
------------
- Red Faction Guerrilla Re-Mars-tered, GOG version. The mod checks the game executable and stays inactive with any
  other build.
- 64-bit Windows 10 or 11.
- For GPU physics: an NVIDIA graphics card, GTX 16 / RTX 20 series or newer, with a current driver. With any other
  graphics card the debris runs on the CPU instead, which copes with fewer pieces.

Install
-------
The mod comes in two archives:
    rfgphys-@VERSION@-with-asi-loader.zip   everything, including Ultimate ASI Loader 9.7.4 (as dinput8.dll)
    rfgphys-@VERSION@-plugin-only.zip       the same without the loader, for a game that already has one
The mod needs Ultimate ASI Loader, any version from 2018 on, loading .asi files from the scripts folder. Other ASI
loaders may load it without starting it.

Extract the archive into the game folder, the one that contains rfg.exe, and let it merge the folders.
It adds:
    dinput8.dll                      Ultimate ASI Loader 9.7.4 (only in the archive with the loader)
    scripts\rfgphys.asi              the mod
    scripts\rfgphys.ini              its settings
    scripts\rfgphysx\                the PhysX process (rfgphysx_host.exe, PhysXGpu_64.dll); keep it next to
                                     rfgphys.asi
    CrashDumps\                      crash reports (see below)

Settings
--------
scripts\rfgphys.ini, explained in the file. Enabled=0 turns the mod off without uninstalling it.

Uninstall
---------
Delete scripts\rfgphys.asi, scripts\rfgphys.ini, scripts\rfgphysx\, scripts\rfgphys_runs\ and CrashDumps\, and
dinput8.dll unless another mod uses it. The mod writes nothing into your saves.

Logs and crash reports
----------------------
Every game session writes scripts\rfgphys_runs\<date-time>-session\rfgphys.log and physx_host.log; the newest 10
sessions are kept. While the CrashDumps folder exists, Ultimate ASI Loader writes a crash dump there (about 60 MB
each) if the game crashes; delete the folder to turn that off. When reporting a problem, include the session folder
and, after a crash, the files from CrashDumps.

Known limitations
-----------------
- Debris lying in an area you leave far behind (roughly half a kilometre and more) is removed when the game unloads
  that area, as the game does with its own debris. Destroyed buildings stay destroyed.
- A few percent of the pieces can fall through the ground just after an area loads, and disappear.
- Very large piles need a fast CPU and graphics card. Tested on an NVIDIA RTX 4090 with an 8-core CPU.

License
-------
rfgphys is released under the MIT License (rfgphys_licenses\rfgphys-MIT.txt).

Third-party components
----------------------
- Ultimate ASI Loader (https://github.com/ThirteenAG/Ultimate-ASI-Loader), MIT License (in the archive with the
  loader)
- MinHook (https://github.com/TsudaKageyu/minhook), BSD 2-Clause License
- NVIDIA PhysX SDK 5.12 (https://github.com/NVIDIA-Omniverse/PhysX), Apache License 2.0
The license texts are in rfgphys_licenses\.

Misc
----
For more game fixes, visit: https://slop-blog.enkhayzomachines.net/fixes :)
