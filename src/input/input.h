#ifndef INPUT_H
#define INPUT_H

#include <stdbool.h>
#include "raylib.h"
#include "player/player.h"

// Client input layer: polls the input library into an abstract PlayerInput and
// builds a PlayerView from the client camera. In multiplayer, a network layer
// would produce these instead.
PlayerInput PollPlayerInput(bool HasControl);
PlayerView PlayerViewFromCamera(Camera3D Camera);

#endif
