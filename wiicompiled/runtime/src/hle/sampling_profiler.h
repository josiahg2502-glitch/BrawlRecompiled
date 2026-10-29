#pragma once

// NSMBW_PROFILE=1 sampling of the game thread (sampling_profiler.cpp). Both are no-ops without the variable.
void SamplingProfilerAttachCurrentThread();
void SamplingProfilerReport();
