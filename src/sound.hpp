#pragma once

namespace sparring {

// Short, soft sounds made in code (nothing to ship): a chime for right, a gentle boop for wrong, a little fanfare for a badge or a
// new level, and a sparkle for the treasure chest.
enum class Sound { Right, Wrong, Cheer, Chest };

void play(Sound s);
void set_sound_on(bool on);

}  // namespace sparring
