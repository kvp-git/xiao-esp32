PNGdec 1.1.6 from https://github.com/bitbank2/PNGdec/tree/1.1.6.

The upstream source and license are kept in `upstream/`. The local CMake wrapper
selects the portable API without Arduino and reserves two RGBA scanlines for
images up to 1024 pixels wide, matching the player's input limit.
