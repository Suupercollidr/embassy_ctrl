enum class InverterAction
{
  TURN_ON,  // Slå på invertern
  TURN_OFF, // Slå av invertern
  NO_CHANGE // Behåll nuvarande status
};

struct InverterMessage {
    InverterAction action;
};