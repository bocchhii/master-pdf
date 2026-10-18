#pragma once

// The Python app's sounds, made by the app itself (no sound files): "done" a soft bell-like
// chime going up, "error" a short warm chord going down. Off in Settings > Sound effects.
namespace sounds {

enum Kind { Done, Error };
void play(Kind kind);  // (without waiting; nothing if sounds are off)

}  // namespace sounds
