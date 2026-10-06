**What's new**

- Less waiting on the GPU. Which parts of a buffer draws and dispatches used was kept rounded out to 256 bytes, so ones next to each other looked like they overlapped, and each got a barrier that waits for the work before it. inFAMOUS Second Son runs hundreds of tiny dispatches a frame that write next to each other, and they all waited. Accesses are now kept to the byte.
- The GPU timing the log reports is measured in fewer command buffers, as writing and reading back the timestamps for each of them took time on the GPU thread and on the GPU.
- Measured on an RTX 5080 with a Ryzen 7 9800X3D, standing in the park in Seattle: 65 -> 70 fps, the GPU busy 14.0 -> 10.5 ms a frame, 1520 -> 1140 barriers a frame, and the game's main thread waiting for GPU results 27% -> 15% of its time. There, the GPU thread is now what holds the frame rate; in fights, where the GPU does, the gain should be larger. Please send a log from a fight.
