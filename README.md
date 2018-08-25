Audiobook Recorder
==================

This is a small program I wrote for recording audio books.

It's unlike most audio recording software, because:

* It operates in manually created chunks
* Chunks are trimmed automatically
* The whole lot can be joined together with room noise between each chunk
* The most recently recorded chunk can be deleted and re-recorded

The operation is simple enough:

1. Load the program optionally giving it an ALSA device name (`-d hw:GoMic`) and a session name (`-d "Chapter 1"`).
2. Press `N` to record 5 seconds of room noise (silence) and begin the session.
3. Hold `R` to record a new chunk.

If you provide a name, and the file `name/room-noise.wav` already exists, you automatically continue the existing
session.

Pressing `D` deletes the most recent chunk.

Each chunk is saved in a file `name/segment-nnnn.wav` and is automatically trimmed to give around 0.1s of room noise
before and after each chunk.

Pressing `C` will export the whole lot with 2 seconds of room noise at the start, then 1 second after each segment.
The results are saved as `name.wav`.

`P` will record a 0.1s pulse of 48kHz tone to be used as a marker within the audio.
Ideal for marking chapters.

`Q` quits.

Command line options:

```
-n <name>                  Specify a name for the session.
                           Also used to resume an existing session.

-d <device>                Specify the ALSA device to record from.
                           Note: it *must* support 48000Hz recording at the moment.

-r <dir>                   Where to save recordings to.  Defaults to
                           $HOME/Recordings

-f                         Enable fullscreen at 320x240

-b                         Enable GPIO button support
```

The last two are intended for running on a 2.1" TFT screen on a Raspberry Pi.
