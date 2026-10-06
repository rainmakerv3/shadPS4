**What's new**

- The GPU thread, which holds the frame rate in busy scenes, does less for each draw again:
  - Finding which variant of a shader a draw needs compared where the shader is in memory, and the game has copies of some shaders at hundreds of addresses. 13% of lookups missed and worked the variant out again; now 0.3% do, with about one comparison a lookup instead of almost three.
  - Binding a texture no longer copies its whole description, nearly 400 bytes, and its lookup key is hashed faster.
  - A shader's hash is no longer read from the game's memory for every draw, buffers bound where they were before no longer search for memory to make resident, and written buffers still marked as written skip a lock that game threads hold while they fault.
  - Descriptor sets of shaders with many resources are written on the recording thread, and register writes and the profiler's counters cost less.
- Measured on an RTX 5080 with a Ryzen 7 9800X3D, standing in the park in Seattle, three times each in turn with the emulator at high priority: 46.9 -> 49.3 fps on average over 1.0.11 with a lot running in the background, and 58.6 -> 62.3 fps with less. The GPU thread still holds the frame rate.
