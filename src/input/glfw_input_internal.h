#ifndef INPUT_GLFW_INPUT_INTERNAL_H
#define INPUT_GLFW_INPUT_INTERNAL_H

// Seam between the GLFW platform and the GLFW input implementations, which
// share one window. Nothing outside those two translation units includes it.

typedef struct GLFWwindow GLFWwindow;

// Registers the input callbacks on the window. Called once, right after the
// platform has created it.
void GlfwInputAttach(GLFWwindow *Window);

// Starts a new input frame: current key and button state becomes previous,
// and the per-frame queues (typed characters, key repeats, scroll) are
// cleared. The platform calls it immediately before pumping window events, so
// everything queried during a frame reflects exactly the events of the pump
// that preceded it.
void GlfwInputBeginFrame(void);

// The platform moved the cursor itself (centring it when capture changes).
// The jump is not a mouse movement, so it must not show up as a look delta.
void GlfwInputWarpCursor(double X, double Y);

#endif
