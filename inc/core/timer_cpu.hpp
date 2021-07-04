
#ifndef TIMER_CPU_HPP_
#define TIMER_CPU_HPP_

#include "timer.hpp"
#include <chrono>
#include <stdexcept>

namespace gearshifft {

  /** CPU Wall timer
   */
  struct TimerCPU_ {
    using clock = std::chrono::high_resolution_clock;

    clock::time_point start;
    double time = 0.0;

    void startTimer() {
      start = clock::now();
    }

    double stopTimer() {
      auto diff = clock::now() - start;
      return (time = std::chrono::duration<double, std::milli> (diff).count());
    }
  };

  using TimerCPU = Timer<TimerCPU_>;
}
#endif /* TIMER_CPU_HPP_ */
