#pragma once

// ManageFrameSplitting: disable frame splitting when FG active + G-Sync,
// re-enable when no longer needed.
// Also: RestoreFrameSplitting for shutdown cleanup.

void ManageFrameSplitting();
void RestoreFrameSplitting();
