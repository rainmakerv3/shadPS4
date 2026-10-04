**What's new**

- The thread that limits the frame rate in fights, the GPU command processor, no longer waits for the graphics driver: the Vulkan commands it makes are now recorded and submitted by a thread of their own. The driver took about a fifth of its time. This is the *Record GPU Commands on a Separate Thread* setting, on by default. If a game misbehaves with it, turn it off there, or set `threaded_command_recording` to `false` in the `GPU` section of `config.json`, and please send the log.
- The log now has the time of that thread ("command recording", and "Sampler: recorder"), and every ten seconds the GPU time of the compute shaders that take the most of it in fights ("Dispatches timed one by one"), to find what to speed up next.
