#pragma once

#include <cstdint>

// Same owner requirement as DoOwnSleep. False means the target was expired,
// invalid, or waiting did not complete (including shutdown); final error is zero.
// The legacy void DoOwnSleep entry point remains available for cap callers.
bool TryOwnSleep(int64_t target_qpc);