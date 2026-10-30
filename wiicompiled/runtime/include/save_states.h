#pragma once

#include <string>

// Save states: the whole game as it is at one frame (guest memory, every guest thread exactly where it stopped, the OS
// timers), kept in memory for the rest of the session and put back on load, from anywhere.
//
// Guest threads are host fibers running translated code, so a thread's state is its host stack as much as its guest
// registers. Every fiber switch goes through SwitchFiber, which leaves the switched-out thread's resume point
// (__builtin_setjmp) in a per-fiber slot on the heap and its registers on its own stack; a snapshot copies each stack
// from that point up to where the thread started (MarkStackTop). A load copies them back and longjmps each thread to
// its saved point the next time it runs. Both happen on a helper fiber, from the main thread's frame boundary, so no
// stack being copied is running. Host addresses differ between runs, so states last until the game is closed, and a
// state only loads while the same guest threads exist (the same match or level, typically).
namespace SaveStates {

constexpr int kSlots = 3;
int Slot();
void SetSlot(int slot);
// "Empty", or when and where it was saved, for the menu.
std::string Describe(int slot);
bool Exists(int slot);
// False in online play (every player's game must stay the same).
bool Available();
// Carried out at the next frame boundary; the result shows as a notice.
void RequestSave();
void RequestLoad();
// A message about the last save or load for the screen, once (empty when there is none).
std::string TakeMessage();

// The game's main thread, once per frame (VISetNextFrameBuffer): carries out a requested save or load.
void FrameBoundary();
// Every guest fiber switch (fiber_manager.cpp).
void SwitchFiber(void* target);
// First thing on each guest fiber (and the main thread before the game starts): its stack above `top` never changes.
void MarkStackTop(void* top);
// A fiber about to be destroyed.
void ForgetFiber(void* fiber);

// gfSceneManager's current scene ("scMelee", "scSelCharacter", ...), "" before there is one.
std::string SceneName();

} // namespace SaveStates
