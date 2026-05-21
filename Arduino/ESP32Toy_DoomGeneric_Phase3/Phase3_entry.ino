// This file is intentionally not the active Arduino sketch entry.
//
// Open this folder using the correctly named sketch file instead:
//
//   ESP32Toy_DoomGeneric_Phase3.ino
//
// Arduino IDE expects the main .ino file name to match the folder name.
// If you open Phase3_entry.ino directly, the IDE may copy it into a new
// Phase3_entry folder and compile only that file, causing linker errors such as:
//
//   undefined reference to `doomgeneric_Create'
//   undefined reference to `doomgeneric_Tick'
//
// The active setup()/loop() implementation lives in:
//
//   ESP32Toy_DoomGeneric_Phase3.ino
