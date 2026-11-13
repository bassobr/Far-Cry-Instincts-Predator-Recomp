# fcp_audio_input.cmake: measurement and test parts of the audio and input path. Included from fcp_boot.cmake.
#  - src/fcp/input_trace.*: what the title receives from XamInputGetState / sends to XamInputSetState;
#  - src/fcp/sound_trace.*: reads of dvdsound.dat (FCP_TRACE_SOUND_READS=1);
#  - src/fcp/virtual_pads.*: virtual SDL3 gamepads for self tests (FCP_VIRTUAL_PADS).
# The mix recording (FCP_RECORD_AUDIO) lives in the SDK copy (rexglue/src/apu, UPSTREAM change 30).

#  - src/fcp/pc_input.*: keyboard/mouse as pad 0, mouse look hook, pad connection state.

target_sources(predator PRIVATE src/fcp/input_trace.cpp src/fcp/sound_trace.cpp src/fcp/virtual_pads.cpp
                                src/fcp/pc_input.cpp)
target_link_libraries(predator PRIVATE SDL3::SDL3)
