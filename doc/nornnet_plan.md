# nornNET

The TCP/IP stack for nornOS lives in its own repository, `../nornNET`, and its design
sketch and phase plan in `nornNET/doc/plan.md`. It attaches to a nornOS application
through the `MODULES` hook of the Makefile: the application's `app.mk` lists
`$(NORNNET_DIR)/nornnet.mk`, and an `nn_config.h` next to the application configures
the stack. `make APP=nn01 APPDIR=../nornNET/apps/nn01 BOARD=esp32` builds its first test
application.
