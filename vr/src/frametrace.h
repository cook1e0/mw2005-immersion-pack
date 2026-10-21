// Per-frame trace ([VR] KeyTrace, default Delete): 30 s of frames, one CSV row each, to see
// what makes the frame time move from frame to frame (the 5 s "perf:" lines only have averages and
// maxima). Measurement only: nothing is drawn differently, and the GPU timestamp queries exist and
// are issued only while a trace runs. Written to nfsmw_frametrace_NNN.csv next to speed.exe by a
// thread of its own when the 30 s are up.
#pragma once
#include <d3d9.h>

// Render thread. Each call is a no-op unless a trace is running.
void Trace_KeyCheck(int vk, IDirect3DDevice9* dev);   // once a frame: starts a trace on the key
bool Trace_On();
void Trace_EyeBegin(int eye);                       // around the eye's scene render (GPU timestamps)
void Trace_EyeEnd(int eye, double cpuSeconds, unsigned draws);
void Trace_BeforeGpuWait();                          // the frame's last GPU command
void Trace_AfterGpuWait();                           // the GPU is idle: this frame's timestamps are ready
void Trace_HudProbe();                               // the HUD probe copied the HUD this frame
// The end of the frame (after Present): phase times in seconds (ProfSlot order, PROF_COUNT of them),
// the frame's length and the part outside the phases.
void Trace_Frame(const double* phases, int count, double frame, double game);
