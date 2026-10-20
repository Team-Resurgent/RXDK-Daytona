#pragma once
constexpr int SCE_TOUCH_PORT_FRONT = 0, SCE_TOUCH_SAMPLING_STATE_START = 1;
struct SceTouchReport { unsigned short x, y; };
struct SceTouchData { unsigned reportNum; SceTouchReport report[8]; };
inline int sceTouchSetSamplingState(int, int) { return 0; }
inline int sceTouchPeek(int, SceTouchData *data, int) { data->reportNum = 0; return 1; }
