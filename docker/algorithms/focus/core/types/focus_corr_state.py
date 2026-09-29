from enum import Enum

class FocusLevel(Enum):
    HighFocused = 3
    MediumFocused = 2
    LowFocused = 1
    Neutral = 0
    LowDistracted = -1
    MediumDistracted = -2
    HighDistracted = -3

class FocusLevelThreshold(Enum):
    HighFocused = 2.277722765
    MediumFocused = 1.721814093
    LowFocused = 1.175777189
    LowDistracted = 0.818128286
    MediumDistracted = 0.43668397
    HighDistracted = 0.134989059

class FocusScoreWeight(Enum):
    PrevWindowWeight = 0.4
    CurrentWindowWeight = 0.6
