# Built the way an SDK application is: no rtos_config.h here, the configuration comes
# from the profile and the kernel from its library (make lib BOARD=... PROFILE=...).
BOARD   = esp32
PROFILE = default
USE_LIB = 1
