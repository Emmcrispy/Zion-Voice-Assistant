#ifndef STATE_MACHINE_H
#define STATE_MACHINE_H

#include "Config.h"

class StateMachine {
private:
  State currentState;
  unsigned long stateStartTime;

public:
  StateMachine();
  void setState(State newState);
  State getState();
  unsigned long getStateTime();
};

#endif
