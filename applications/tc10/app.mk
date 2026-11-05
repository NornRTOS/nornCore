# Kernel benchmark: needs the cycle counter and probe timer of hal/<arch>/board.h, which
# the UNO cannot offer (timer 1 is its tick and it has no timer 4).
BOARD = mega2560
ONLY_BOARDS = mega2560 esp32
