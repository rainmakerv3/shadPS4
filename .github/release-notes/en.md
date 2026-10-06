**What's new**

- The GPU thread, which holds the frame rate in busy scenes, does less for each draw again:
  - Buffers a draw or dispatch writes, 1400-1600 a frame, are no longer looked up in a tree to find out they are marked as written already: the last few marked are remembered.
  - Binding a buffer checked twice whether anything written since the last barrier overlaps it, over 100000 times a frame. Small buffers read, which the buffer cache checks already, are now checked once.
  - Packing the descriptors of a draw for the recording thread copies them in place instead of calling memcpy for each.
- Measured on an RTX 5080 with a Ryzen 7 9800X3D, standing in the park in Seattle, in turn with the emulator at high priority: 66.9 -> 70.1 fps on average over 1.0.13 in three rounds, with 41-44% of the CPU taken by other programs. The GPU thread, the host GPU and the game's main thread are now all close to their limits there: the GPU thread waits for the game a few percent of the time, and the host GPU is busy 12-13 ms of each frame.
