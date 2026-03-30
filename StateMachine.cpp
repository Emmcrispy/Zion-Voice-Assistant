#include "StateMachine.h"

StateMachine::StateMachine()
  : currentState(STATE_IDLE_LISTENING), stateStartTime(0) {}

void StateMachine::setState(State newState) {
    if (newState == currentState) return;
    currentState   = newState;
    stateStartTime = millis();

    const char* names[] = {
        "IDLE_LISTENING",
        "WAKE_DETECTED",
        "PLAYING_ACK",
        "RECORDING",
        "PROCESSING_STT",
        "THINKING_LLM",
        "SPEAKING_TTS",
        "FOLLOWUP_PROMPT",
        "FOLLOWUP_LISTENING",
        "PROCESSING_FOLLOWUP",
        "PLAYING_LISTENING",
        "PLAYING_GOODBYE",
        "GENERATING_IMAGE",
        "SHOWING_IMAGE",
        "ACTIVE_LISTENING",
        "ERROR"
    };
    const int nameCount = sizeof(names) / sizeof(names[0]);
    int idx = (int)newState;
    if (idx >= 0 && idx < nameCount) {
        Serial.printf("── STATE: %s ──\n", names[idx]);
    }
}

State StateMachine::getState() {
    return currentState;
}

unsigned long StateMachine::getStateTime() {
    return millis() - stateStartTime;
}