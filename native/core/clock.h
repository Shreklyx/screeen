// Screen Share native core – the clock the protocol uses: milliseconds since 1970 (as the pages'
// performance.timeOrigin + performance.now()), but monotonic: read once from the system clock,
// then advanced by the steady clock so it never jumps.
#pragma once

namespace ss {

double nowMs();

}  // namespace ss
